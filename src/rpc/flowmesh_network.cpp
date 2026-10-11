// Copyright (c) 2026 The B3Coin Core developers
// Distributed under the MIT software license, see COPYING.

#include <node/context.h>
#include <node/flowmesh_client.h>
#include <node/flowmesh_health.h>
#include <node/flowmesh_service.h>
#include <node/flowmesh_timing.h>
#include <chainparams.h>
#include <rpc/server_util.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <univalue.h>
#ifdef ENABLE_FLOWMESH_P2FV_BOOTSTRAP_TEST
#include <test/flowmesh_p2fv_bootstrap.h>
#endif

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

static RPCResult TargetChannelResult(const std::string& name)
{
    return {RPCResult::Type::OBJ, name, "Connection-attempt observation, not application delivery", {
        {RPCResult::Type::STR, "state", "Current local connection/retry state"},
        {RPCResult::Type::NUM, "attempts", "Outgoing connection attempts during this process"},
        {RPCResult::Type::NUM, "failures", "Observed failed attempts or connection losses"},
        {RPCResult::Type::STR, "last_error", "Most recent explicit local failure reason; may remain after recovery"},
        {RPCResult::Type::NUM, "retry_in_ms", "Local retry delay in milliseconds; eligibility is not a connection promise"},
        {RPCResult::Type::NUM, "last_attempt_age_ms", "Milliseconds since last attempt, or -1 when none"},
        {RPCResult::Type::BOOL, "authenticated", "This channel completed challenge authentication"},
    }};
}

static UniValue TargetChannelJSON(const node::FlowMeshNetTargetChannel& channel)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("state", channel.state);
    out.pushKV("attempts", channel.attempts);
    out.pushKV("failures", channel.failures);
    out.pushKV("last_error", channel.last_error);
    out.pushKV("retry_in_ms", channel.retry_in_ms);
    out.pushKV("last_attempt_age_ms", channel.last_attempt_age_ms);
    out.pushKV("authenticated", channel.authenticated);
    return out;
}

static RPCHelpMan reconnectflowmeshclient()
{
    return RPCHelpMan{
        "reconnectflowmeshclient",
        "Retry the ordinary trader's configured HTTPS endpoints with a fresh, read-only markets request. "
        "No wallet or unlock is required. Does not sign, submit/retry/cancel an action, clear saved history, change endpoints/trust, or enable a validator. "
        "Uses the existing verified TLS and bounded failover policy (up to eight sequential attempts, each with a five-second transport deadline). "
        "Returns busy immediately if another client request is running; no overlapping worker is started. "
        "Reachable means an API response was obtained, not quorum, certification, synced state or a persistent socket.\n",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "Read-only reconnect observation", {
            {RPCResult::Type::STR, "status", "reachable, unavailable, busy, not_configured or not_remote"},
            {RPCResult::Type::BOOL, "endpoint_available", "A configured HTTPS endpoint answered this probe successfully"},
            {RPCResult::Type::STR, "endpoint", "Successful configured endpoint for this call; empty otherwise"},
            {RPCResult::Type::NUM, "attempted_endpoints", "HTTPS attempts made by this call"},
            {RPCResult::Type::NUM, "actions_submitted", "Always zero; this command never submits an action"},
            {RPCResult::Type::BOOL, "availability_is_certification", "Always false"},
            {RPCResult::Type::STR, "error", "Bounded failure/busy/configuration reason, or empty"},
        }},
        RPCExamples{HelpExampleCli("reconnectflowmeshclient", "")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto& backend{EnsureAnyNodeContext(request.context).flowmesh_trading};
            if (!backend) throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "FlowMesh trading backend is not available yet");
            const auto result{backend->Reconnect()};
            UniValue out{UniValue::VOBJ};
            out.pushKV("status", result.status);
            out.pushKV("endpoint_available", result.endpoint_available);
            out.pushKV("endpoint", result.endpoint);
            out.pushKV("attempted_endpoints", uint64_t{result.attempted_endpoints});
            out.pushKV("actions_submitted", 0);
            out.pushKV("availability_is_certification", false);
            out.pushKV("error", result.error);
            return out;
        }};
}

static RPCHelpMan flowmeshconnect()
{
    return RPCHelpMan{
        "flowmeshconnect",
        "Add a pinned peer to the running independent FlowMesh network without restarting. "
        "Admission is not connection: use getflowmeshnetworkinfo for per-target retry/error and authentication status. "
        "The target is memory-only; add the same flowmeshconnect= entry to your existing configuration if it must survive restart. "
        "Does not change listening/firewall settings, arm FN keys, or prove quorum.\n",
        {{"peer", RPCArg::Type::STR, RPCArg::Optional::NO,
          "Other operator's 66-hex compressed network public key@numeric-IP:port (IPv6 uses [address]:port); not an FN BLS key"}},
        RPCResult{RPCResult::Type::OBJ, "", "Local connection-target admission", {
            {RPCResult::Type::BOOL, "accepted", "Target was admitted or was already present; not a connection result"},
            {RPCResult::Type::BOOL, "already_present", "Exact target was already registered; no duplicate retry work was created"},
            {RPCResult::Type::STR, "status", "queued, already_present, or refused"},
            {RPCResult::Type::STR, "address", "Validated target endpoint, or empty on validation failure"},
            {RPCResult::Type::STR, "operator_pubkey", "Pinned public network identity, or empty on validation failure"},
            {RPCResult::Type::STR, "persistence", "memory_only; this call never edits startup configuration"},
            {RPCResult::Type::BOOL, "connection_is_quorum_proof", "Always false"},
            {RPCResult::Type::STR, "error", "Precise admission refusal, or empty"},
        }},
        RPCExamples{HelpExampleCli("flowmeshconnect", "\"<operator_pubkey>@<numeric_ip>:5649\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto& service{EnsureAnyNodeContext(request.context).flowmesh};
            if (!service) throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "FlowMesh service is not available yet");
            const auto result{service->AddNetworkPeer(request.params[0].get_str())};
            UniValue out{UniValue::VOBJ};
            out.pushKV("accepted", result.accepted);
            out.pushKV("already_present", result.already_present);
            out.pushKV("status", !result.accepted ? "refused" : result.already_present ? "already_present" : "queued");
            out.pushKV("address", result.address);
            out.pushKV("operator_pubkey", result.operator_pubkey);
            out.pushKV("persistence", "memory_only");
            out.pushKV("connection_is_quorum_proof", false);
            out.pushKV("error", result.error);
            return out;
        }};
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
            {RPCResult::Type::ARR, "targets", "Bounded configured and runtime-added connection targets, including unsuccessful attempts", {
                {RPCResult::Type::OBJ, "", "", {
                    {RPCResult::Type::STR, "address", "Numeric destination endpoint"},
                    {RPCResult::Type::STR, "operator_pubkey", "Pinned identity, or empty for an unpinned startup target"},
                    {RPCResult::Type::BOOL, "runtime_added", "Added through RPC rather than startup configuration"},
                    {RPCResult::Type::BOOL, "admitted_to_worker", "Worker has installed this queued target"},
                    {RPCResult::Type::BOOL, "authenticated", "Logical peer completed all required transport channels"},
                    TargetChannelResult("critical"),
                    TargetChannelResult("action"),
                    TargetChannelResult("bulk"),
                }},
            }},
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
            UniValue targets{UniValue::VARR};
            for (const auto& target : snapshot.targets) {
                UniValue item{UniValue::VOBJ};
                item.pushKV("address", target.address);
                item.pushKV("operator_pubkey", target.operator_pubkey);
                item.pushKV("runtime_added", target.runtime_added);
                item.pushKV("admitted_to_worker", target.admitted_to_worker);
                item.pushKV("authenticated", target.authenticated);
                item.pushKV("critical", TargetChannelJSON(target.channels[0]));
                item.pushKV("action", TargetChannelJSON(target.channels[1]));
                item.pushKV("bulk", TargetChannelJSON(target.channels[2]));
                targets.push_back(std::move(item));
            }
            out.pushKV("targets", std::move(targets));
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
                    {RPCResult::Type::NUM, "agreement_duplicates_coalesced", "Exact repeats of already verified preliminary-agreement bytes consumed at admission; not receipt of anything new"},
                    {RPCResult::Type::NUM, "certified_slot_frames_consumed", "Committee frames for slots this node already appended (older votes and agreement messages, exact copies of its recent certified entries) consumed at admission without a queue slot or verification; not receipt of anything new"},
                    {RPCResult::Type::NUM, "settled_frames_rate_limited", "Frames of the two kinds above refused because their sender exceeded a separate budget for consumed frames (256 burst and 64 per second per peer and market, 512 burst and 128 per second per peer across all markets). A peer is one connection identity (legacy NodeId or FMN2 session id); a reconnect or dual carriage gets a fresh budget. In steady state the per-peer budget binds when one sender's summed sustained rate of such frames exceeds 128 per second, about 11 markets at the 12 per second average measured with four validators; a non-zero value means a budget binds"},
                    {RPCResult::Type::NUM, "deferred_objects", "Currently retained critical incoming objects"},
                    {RPCResult::Type::NUM, "deferred_bytes", "Bytes charged to retained critical incoming objects"},
                    {RPCResult::Type::OBJ, "action_admission", "ACTION frame header policy (local relay policy, never consensus). A frame's header only claims its sender's certified head", {
                        {RPCResult::Type::NUM, "header_refused_identity", "Remote frames for another version, market or epoch"},
                        {RPCResult::Type::NUM, "header_refused_far", "Remote frames more than two slots ahead of or behind this node's head"},
                        {RPCResult::Type::NUM, "header_refused_unproven", "Remote frames behind this head whose interval this node could not prove excludes the action"},
                        {RPCResult::Type::NUM, "header_accepted_ahead", "Frames admitted from a sender one or two slots ahead of this node"},
                        {RPCResult::Type::NUM, "header_restamped_behind", "Frames admitted from a sender one or two slots behind, relayed with this node's head"},
                    }},
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
                item.pushKV("agreement_duplicates_coalesced", snapshot.agreement_duplicates_coalesced);
                item.pushKV("certified_slot_frames_consumed", snapshot.certified_slot_frames_consumed);
                item.pushKV("settled_frames_rate_limited", snapshot.settled_frames_rate_limited);
                item.pushKV("deferred_objects", uint64_t{snapshot.deferred_objects});
                item.pushKV("deferred_bytes", uint64_t{snapshot.deferred_bytes});
                UniValue action_admission{UniValue::VOBJ};
                action_admission.pushKV("header_refused_identity", snapshot.action_admission.header_refused_identity);
                action_admission.pushKV("header_refused_far", snapshot.action_admission.header_refused_far);
                action_admission.pushKV("header_refused_unproven", snapshot.action_admission.header_refused_unproven);
                action_admission.pushKV("header_accepted_ahead", snapshot.action_admission.header_accepted_ahead);
                action_admission.pushKV("header_restamped_behind", snapshot.action_admission.header_restamped_behind);
                item.pushKV("action_admission", std::move(action_admission));
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

static std::vector<RPCResult> SeatEvidenceStatementResult()
{
    using Type = RPCResult::Type;
    return {
        {Type::NUM, "seat_epoch", "Seat epoch of the signed certificate context"},
        {Type::STR_HEX, "seat_set_hash", "Anchored seat set of that epoch"},
        {Type::NUM, "sequence", "Microblock sequence"},
        {Type::STR_HEX, "microblock_hash", "Signed microblock hash"},
        {Type::NUM, "seat_index", /*optional=*/true, "attestation_equivocation: the signing seat's index in the set"},
        {Type::STR_HEX, "signer_bitmap", /*optional=*/true, "certificate_conflict: LSB-first signer bitmap"},
        {Type::STR_HEX, "signature", "BLS signature (one seat's, or the certificate's aggregate) over the B3/FLOWMESH/CERT/V1 digest of domain, market_id, seat_epoch, seat_set_hash, sequence and microblock_hash"},
    };
}

static RPCResult HealthResult()
{
    using Type = RPCResult::Type;
    const RPCResult codes{Type::STR, "", "Not-ready code; treat any code you do not know as not ready"};
    return {Type::OBJ, "", "Local, advisory health observation", {
        {Type::NUM, "health_version", "Health object version, currently 1"},
        {Type::STR, "observation", "Always local-node: what this node observed, never certified evidence or a proof of the network's freshest head"},
        {Type::BOOL, "ready", "service_ready and every listed market ready; a label only, market rows decide per-market routing"},
        {Type::BOOL, "service_ready", "No service-level not-ready code"},
        {Type::ARR, "not_ready", "Service-level codes: service_stopped, initial_block_download, below_activation, reconciling_prolonged, worker_stalled, no_flowmesh_peers", {codes}},
        {Type::OBJ, "service", "", {
            {Type::STR, "state", "running, stopped or dormant (the A2/A3 schedule is incomplete)"},
            {Type::STR, "chain_gate", /*optional=*/true, "B3 reconciliation gate while running: open or closed"},
            {Type::STR, "chain_gate_reason", /*optional=*/true, "open, starting, initial_block_download, below_activation, reconciling, idle_wait_timeout, store_reconciliation_failed, market_refresh_failed, checkpoint_reconciliation_failed or tip_changed"},
            {Type::NUM, "chain_gate_closed_ms", /*optional=*/true, "Local milliseconds the gate has been continuously closed"},
            {Type::NUM, "worker_observation_age_ms", /*optional=*/true, "Local milliseconds since the runtime worker last published an observation; grows while the worker is stalled"},
            {Type::NUM, "flowmesh_peers", /*optional=*/true, "Connected FlowMesh peers (operator detail)"},
            {Type::NUM, "no_peers_ms", /*optional=*/true, "Local milliseconds without any FlowMesh peer (operator detail)"},
        }},
        {Type::OBJ, "policy", "Node-local thresholds, never consensus", {
            {Type::NUM, "reconcile_grace_ms", "Continuous gate closure before reconciling_prolonged"},
            {Type::NUM, "lag_grace_ms", "Catch-up duration before catching_up"},
            {Type::NUM, "worker_stall_ms", "Observation age before worker_stalled"},
            {Type::NUM, "no_peers_grace_ms", "Time without FlowMesh peers before no_flowmesh_peers"},
            {Type::NUM, "stall_grace_ms", "Time with includable signed work and no head advance before stalled_with_pending_work"},
            {Type::NUM, "live_window_ms", "Label only: a head applied within this window is live, otherwise idle"},
        }},
        {Type::ARR, "markets", "At most 256 markets, in market id order", {
            {Type::OBJ, "", "", {
                {Type::STR_HEX, "market_id", "Market identity"},
                {Type::BOOL, "ready", "No not-ready code for this market"},
                {Type::ARR, "not_ready", "Service codes plus market_not_installed, market_halted, catching_up or stalled_with_pending_work", {codes}},
                {Type::STR, "state", "One label: stopped, not_installed, halted, inactive, reconciling, worker_stalled, isolated, insufficient_seats, handoff_pending, paused, stalled, catching_up, live or idle"},
                {Type::STR, "halt", /*optional=*/true, "Runtime halt code, or none"},
                {Type::BOOL, "paused", /*optional=*/true, "Runtime pause"},
                {Type::STR, "pause_reason", /*optional=*/true, "Fixed pause reason, or empty; b3_reconciliation_pending is the normal per-block pause"},
                {Type::BOOL, "pending_handoff", /*optional=*/true, "A committee handoff waits for its anchor depth"},
                {Type::NUM, "epoch", /*optional=*/true, "Current seat epoch"},
                {Type::NUM, "active_seats", /*optional=*/true, "Active seat count"},
                {Type::NUM, "quorum_required", /*optional=*/true, "Signatures a certificate needs"},
                {Type::NUM, "pending_actions", /*optional=*/true, "Actions held in this node's pool"},
                {Type::NUM, "next_microblock_sequence", /*optional=*/true, "Next sequence this node will apply"},
                {Type::STR_HEX, "last_microblock_hash", /*optional=*/true, "Hash of this node's certified head, or zero"},
                {Type::NUM, "head_age_ms", /*optional=*/true, "Local milliseconds since this process applied its head; idle markets age legitimately"},
                {Type::NUM, "work_waiting_ms", /*optional=*/true, "Local milliseconds this pool has continuously held an includable signed action; absent when it holds none"},
                {Type::NUM, "head_local_observed_unix", /*optional=*/true, "Local wall-clock seconds when this node applied its head; an observation, not certified time"},
                {Type::OBJ, "catchup", /*optional=*/true, "Catch-up observation", {
                    {Type::BOOL, "behind", "Proven or speculative lag"},
                    {Type::NUM, "behind_ms", /*optional=*/true, "Local milliseconds continuously behind"},
                    {Type::NUM, "proven_target", /*optional=*/true, "One past the highest BLS-verified certificate this node has not applied yet; cannot be hidden"},
                    {Type::NUM, "proven_behind_ms", /*optional=*/true, "Local milliseconds continuously provably behind; only this lag makes the market catching_up"},
                    {Type::BOOL, "speculative", "A verified catch-up page arrived without tail slack; not a proof and never a readiness input"},
                    {Type::NUM, "peer_hint_next_sequence", /*optional=*/true, "Highest unverified peer head announcement (operator detail)"},
                    {Type::NUM, "peers_hinting_ahead", /*optional=*/true, "Peers announcing a head ahead of ours, unverified (operator detail)"},
                }},
                {Type::OBJ, "seat_evidence", /*optional=*/true, "Seat-misbehaviour evidence this node recorded for the market. Information only: "
                    "never a readiness input, never a penalty, never consensus. Each record carries every signed field and is checkable against "
                    "the anchored seat set each statement names", {
                    {Type::NUM, "recorded", "Records ever kept in this market's store, evicted ones included"},
                    {Type::NUM, "retained", "Records the store still holds, at most 64"},
                    {Type::NUM, "unrecorded", /*optional=*/true, "Detections this process could not record: not verifiable against this node's chain, over the per-minute check pace (applied before any verification), or a failed write (operator detail)"},
                    {Type::ARR, "latest", "Newest first: every retained record with operator detail, otherwise at most 4", {
                        {Type::OBJ, "", "", {
                            {Type::STR_HEX, "evidence_id", "Order-independent record identity"},
                            {Type::STR, "kind", "attestation_equivocation (one seat signed two microblocks at one epoch and sequence) or certificate_conflict (two valid threshold certificates for two microblocks at one sequence)"},
                            {Type::STR_HEX, "domain", "Chain domain every statement signs"},
                            {Type::STR_HEX, "market_id", "Market every statement signs"},
                            {Type::OBJ, "first", "The statement this node held first (its earlier vote or durable entry)", SeatEvidenceStatementResult()},
                            {Type::OBJ, "second", "The conflicting statement it received", SeatEvidenceStatementResult()},
                            {Type::STR_HEX, "seat_public_key", /*optional=*/true, "attestation_equivocation: BLS public key of the seat at seat_index in the named set; compare with your own keys (getflowmeshvalidatorinfo)"},
                            {Type::ARR, "double_signers", /*optional=*/true, "certificate_conflict under one seat set: the seats in both signer bitmaps, each of which signed both", {
                                {Type::NUM, "", "Seat index"},
                            }},
                        }},
                    }},
                }},
                {Type::STR, "error", /*optional=*/true, "Bounded runtime error text (operator detail)"},
                {Type::STR, "proposal_wait_reason", /*optional=*/true, "Why the next slot's proposal waits (operator detail)"},
            }},
        }},
        {Type::BOOL, "markets_truncated", "More markets exist than listed"},
    }};
}

static RPCHelpMan getflowmeshhealth()
{
    return RPCHelpMan{
        "getflowmeshhealth",
        "Read this node's local FlowMesh health without a wallet: per-market certified head, local head age, halt and pause state, "
        "seat counts, catch-up lag and advisory endpoint readiness, with operator detail. The public HTTPS 'health' method serves "
        "the same object without operator detail. A local observation, never certified evidence, a proof of the network's freshest "
        "head, or an input to admission, relay, signing or verification.\n",
        {{"market_id", RPCArg::Type::STR_HEX, RPCArg::Optional::OMITTED, "Limit to one market; otherwise all markets"}},
        HealthResult(),
        RPCExamples{HelpExampleCli("getflowmeshhealth", "") + HelpExampleCli("getflowmeshhealth", "\"<market_id>\"")},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            const auto& service{EnsureAnyNodeContext(request.context).flowmesh};
            if (!service) throw JSONRPCError(RPC_MISC_ERROR, "FlowMesh engine is not enabled on this node; see getflowmeshclientinfo");
            std::optional<flowmesh::MarketId> market;
            if (!request.params[0].isNull()) market = ParseHashV(request.params[0], "market_id");
            const auto health{service->Health(market)};
            if (market && !health.Find(*market)) throw JSONRPCError(RPC_INVALID_PARAMETER, "Unknown FlowMesh market");
            return node::FlowMeshServiceHealthJson(health, /*operator_detail=*/true);
        }};
}

static RPCHelpMan flowmeshtiming()
{
    return RPCHelpMan{
        "flowmeshtiming", "Regtest-only bounded memory diagnostics; never changes protocol state. Stop before reading.\n",
        {{"command", RPCArg::Type::STR, RPCArg::Optional::NO, "start, stop or read"}},
        RPCResult{RPCResult::Type::OBJ, "", "Capture metadata and bounded events", {}, true},
        RPCExamples{""},
        [&](const RPCHelpMan&, const JSONRPCRequest& request) -> UniValue {
            if (Params().GetChainType() != ChainType::REGTEST)
                throw JSONRPCError(RPC_MISC_ERROR, "Timing capture is only available on regtest");
            try { return node::FlowMeshTimingControl(request.params[0].get_str()); }
            catch (const std::exception& e) { throw JSONRPCError(RPC_INVALID_PARAMETER, e.what()); }
        }};
}

void RegisterFlowMeshNetworkRPCCommands(CRPCTable& table)
{
    static const CRPCCommand commands[]{
        {"hidden", &flowmeshtiming},
        {"flowmesh", &reconnectflowmeshclient},
        {"flowmesh", &flowmeshconnect},
        {"flowmesh", &getflowmeshnetworkinfo},
        {"flowmesh", &getflowmeshdeliveryinfo},
        {"flowmesh", &getflowmeshhealth},
    };
    for (const auto& command : commands) table.appendCommand(command.name, &command);
#ifdef ENABLE_FLOWMESH_P2FV_BOOTSTRAP_TEST
    RegisterFlowMeshP2fvBootstrapRPCCommands(table);
#endif
}

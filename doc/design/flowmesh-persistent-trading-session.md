# FlowMesh persistent trading session — proposed design, revision 1

Date: 2026-09-27. Branch: `flowmeshV2-dev`.
Inspected source: `e4f83139e637ff1d291118cd705130d2855ea5b7`.

Status: original design reference, with the first bounded reuse stage now
implemented at `c81cdb2bc270db0f4721c55cb15e759c588d8202`. See the separate
[qualification report](../flowmesh-https-reuse-qualification.md) for what was
actually built and measured. The two-lane backend, push/live-result path,
fully asynchronous server and later policies below remain proposals. This
document must not be read as qualification of that entire design. No running
tester endpoint/client was upgraded and no economic rule was changed.

Owner clarification: use the peer-style lifecycle — handshake, verify, retain
the connection, exchange messages, reconnect on failure. This is a connection
lifecycle analogy, not permission to reuse FN credentials or replace HTTPS
with the validator protocol. A trader does not become a validator. TLS
authenticates the API server; each economic instruction retains its own
authorization, and each result retains its independent proof requirements.

## 1. Outcome and honest baseline

Keep a verified HTTPS session warm, prioritize trading over optional refreshes,
and deliver existing certified evidence promptly. Preserve ordinary Qt trading
with `enableflowmeshvalidator=0`. Do not replace Qt, matching, the validator
network, quorum, certificates, fees, persistence or settlement in this task.

The recent six-fill regtest campaign measured 4,398.264 ms median from original
Mac order RPC to locally verified inclusion; range 4,026.337–7,662.093 ms.
It used a remote VPS endpoint and four validators colocated on that VPS, not
an all-local run or four independently hosted WAN validators. The Mac source
was the revision above; the VPS source was
`a638fbff07942d12abcd5e183ef7d0bc1b9ba257`. These are the existing native
V1/pre-agreement execution path, not the isolated V2 agreement model integrated
into the daemon. All six fills were corroborated against native replica history
and account deltas. Inclusion alone does not independently prove a fill.

Each fill used one subsequent status RPC, taking 844.093–3,364.686 ms. These
are complete call durations, not isolated TLS, consensus or network timings.
There was no matched instrumented control in this campaign. Do not attribute
the 4.40 s to handshakes or combine it with earlier campaigns' stage medians.
The local 200 ms goal remains a goal; no speedup is established by this design.

Pre-change code-grounded constraints at the inspected baseline (paths relative
to the repository; the qualification report records the implemented delta):

- `src/node/flowmesh_https.cpp::FlowMeshHttpsRequest` creates a TLS context,
  event/DNS bases, SSL and HTTP connection per call and requests closure.
- `FlowMeshHttpsServer::Impl::Serve` handshakes, reads one request, calls one
  handler and closes. Default options have two workers and 64 connections.
- `ReadRequest` requires strict Content-Length framing and rejects pipelining,
  transfer encoding, duplicate header names and surplus request bytes.
- `HandshakeInfo` currently marks possible delivery at handshake completion.
  `SSL` callback data points at a stack-local `ClientCall`.
- `src/node/flowmesh_client.cpp::RemoteBackend` holds `m_work` over a complete
  backend operation, including HTTPS, verification and synchronous journal
  writes. Priority changes queue order, not in-flight duration.
- `Send` saves exact bytes and conservative uncertainty before transmission.
  `QueryAction` verifies canonical inclusion; `previously_certified` prevents
  resubmission even when fresh authority evidence is unavailable.
- `Refresh` consumes cursor pages, treats events as hints, detects gaps and
  derives account state from verified snapshots, not adjacent endpoint rows.

## 2. One logical session, not necessarily one socket

First implement sequential HTTP/1.1 connection reuse, without pipelining, on
the existing restricted API. A verified connection may carry multiple calls;
it is not a promise of one handshake for the application's entire lifetime.
Idle expiry, peer closure, trust changes and reconnection require a new one.

The target design then has at most two connections to the selected endpoint:

- **Trading lane:** fresh review prerequisites, authorized submissions,
  cancellations already signed by the owner, and explicit action recovery.
- **Update lane:** passive reads, history and later live notifications/proofs.

Two warm lanes prevent a long update response from occupying the same HTTP/1.1
request slot needed by a trade. They share bounded resources and do not remove
all CPU, lock, bandwidth or server contention. A label supplied by a remote
client cannot grant unlimited priority or prove trading authority.

Initially connection reuse retains the existing exclusive `m_work` contract:
this deliberately does **not** claim parallel refresh/trade progress. Before
activating the second lane, introduce a separately reviewed split between
asynchronous transport and serialized backend state mutation. Do not merely
add another Qt worker accessing the same mutable caches/outbox.

Use one active endpoint pool, bounded old-connection draining and sequential
failover. Do not multiply two always-warm connections by every configured
endpoint or every market. No HTTP/2, WebSocket, QUIC or compression dependency
is required for the first reuse experiment. Select and version the later watch
framing explicitly before its implementation; do not silently turn arbitrary
RPC methods into a public stream.

## 3. Transport ownership and admission

Server connections need a bounded nonblocking I/O owner for handshake, framing,
idle waiting and response writes. Handler workers receive complete, admitted
requests; they must not spend their lifetime waiting on an idle keep-alive
socket or a subscription. Reuse existing libevent/OpenSSL primitives where
suitable, but retain strict parsing and the public method allowlist.

Separate limits for connection count, handshake work, request bytes, queued
handlers, response bytes, subscriptions, verification jobs and total buffered
bytes. Count both queued and in-flight work. A finite per-connection cap alone
is insufficient: keep global caps as well. Account serialized buffers honestly;
that is not a measurement of all process heap or TLS-library allocations.
Charge method admission/rate limits for every request, including later requests
on a warm connection, not just the initial handshake. A retained connection
never bypasses the action authenticator or the public method allowlist.

Retain current limits until a versioned isolated test profile justifies other
values. The existing 64-connection default cannot qualify 100 simultaneously
connected two-lane clients. Do not quietly raise it or claim that load tested.

Use absolute request deadlines including ready/enqueue wait; trickle bytes
must not continually extend a request. Bound handshake, idle, write and drain
phases separately. Idle expiry affects transport only, not an order's lifetime.
Existing bounded rejection cleanup remains mandatory: reject before handler
dispatch, send a best-effort error, discard only within byte/time budgets and
close. Never reuse an ambiguously framed or rejected connection.

Start with one outstanding request per connection. Retain no-pipelining and
strict length rules; a persistent parser must explicitly handle leftovers and
never reinterpret surplus bytes as an authorized next request. Respect peer
`Connection: close`. Changing a header alone is not a valid implementation.

Pool identity includes normalized endpoint, certificate trust/pin policy and
its generation. Trust reload closes/invalidate old sessions. Certificate
expiry bounds reuse; new handshakes verify CA, hostname/IP and any extra pin.
No plaintext fallback, redirect trust, disabled certificate checks or 0-RTT
economic requests. Pool callbacks reference session-owned state, never the
old stack-local `ClientCall`. Request contexts have independent bounded lives.

**Required warm-connection repair:** mark possible delivery for every request
before its bytes can enter the writer. A handshake callback cannot do this for
the second request. Preserve the existing durable write-ahead uncertainty and
conservative ambiguity even if later socket completion is unavailable.

## 4. Trading, proofs and live updates

Keep these stages distinct: locally saved, possibly sent, endpoint queued,
runtime admitted, certified inclusion, verified account state, supported fill
evidence, B3 checkpoint and payout. A live connection establishes none of the
economic stages. A transport acknowledgment is not a trade confirmation.

Proposed completion path:

1. The existing wallet review/signing flow creates an authorized instruction.
   Preserve exact signed bytes, ActionId, sequence and original submission time.
2. Synchronously save the instruction and uncertainty before transmission.
3. Send on the warm trading lane; report only the actual admission stage.
4. A live update signals change; when bounded evidence is available, deliver
   the **existing canonical certified payload**, not a new proof format.
5. The client runs existing identity, anchor, quorum, inclusion and rollback
   checks, durably preserves no-resubmit/high-water records and only then
   exposes verified state. If only a hint fits, retrieve the same evidence by
   existing status/snapshot methods rather than inventing a success claim.

A subscription must install its bounded cursor capture before acknowledging
the watch, then replay from the captured boundary. This closes the race where
certification occurs between the first snapshot and subscription activation.
Always reconcile already-pending ActionIds on attachment; live events alone
cannot recover a completion emitted before the subscription existed.

Retain the existing 2,048-event ring, 256-event page bound and explicit gap
semantics. Scope cursors to endpoint/runtime instance, market and account.
Endpoint change, restart, scope switch, overflow or skipped events require a
visible gap and verified recovery. Do not advance a cursor past unprocessed
pages. Receipt hints may be coalesced; indispensable signed instructions and
proof/high-water protections must not be evicted. Slow subscribers get bounded
resynchronization or disconnect, never block validator persistence/signing.

Subscriptions are not permission to expose wallet files, seeds, private keys,
local outboxes or owner-only metadata. Do not log request bodies or account
watch lists. Exact public account/ActionId filtering is not authentication or
proof of ownership; keep exposure no broader than the existing public API.
Avoid account or signed instruction data in URLs and access logs.

Push does not prove the freshest network head, validate endpoint-reported fill
history, or remove the need for a verified account snapshot. Maintain these
separate labels. Full proof copies, decodes and BLS verification remain bounded
and deduplicated by full canonical identity; no unbounded per-event tasks.

## 5. State serialization, reconnect and Qt lifetime

The target asynchronous backend retains a **single state owner**. It schedules
bounded immutable network jobs, releases ownership while they wait, and applies
their results through serialized checked completions. A completion carries:
wallet/backend generation, exact market/domain/configuration, account, ActionId
when applicable, request kind and endpoint/trust generation. Recheck current
authority and high-water constraints before applying it. Never advance a nonce
or overwrite a newer snapshot just because a late response finally arrived.

Foreground intent remains prioritized with the existing bounded fairness
principle. CPU verification, journal writes and currently executing handlers
are not magically preemptible. Keep review-time freshness/balance/identity
checks; do not substitute an old displayed snapshot to save a round trip.

On disconnect, local history remains authoritative about what was signed and
possibly sent. The transport reports failure/ambiguity; it never automatically
replays an economic POST. Reconnection performs read-only reconciliation under
existing retry/backoff budgets. Only the existing authorized exact-action retry
path may resend original bytes with the same ActionId/sequence. Absence from
one endpoint is not proof of rejection. A prior certificate retains its
no-resubmit protection even when current verification is pending. Reconnecting,
switching wallets or closing Qt never creates a cancellation or replacement.

Bound notification queues and apply incremental Qt updates on its UI thread.
Preserve typed values, focus, scroll and selected request. Hidden panels detach
display subscriptions without discarding durable pending instructions. Late
results may update the correctly owned backend record, not another wallet's
selected card. Staleness/readiness remains explicit; no heartbeat-only renewal
of certified snapshot freshness.

Shutdown order: stop new intents; detach UI subscriptions and invalidate display
generations; cancel bounded transport I/O/timers; finish required synchronous
state writes and drain owned completions; release backend/wallet references;
destroy the session. Close idle/event sockets promptly. Do not claim a five-
second total shutdown from a per-attempt deadline or silently change the
existing admitted-handler behavior. A blocked storage operation is a separate
failure; cancellation of network work does not excuse an undurable record.

## 6. Small independently testable stages

1. **Reuse plus attribution:** persistent sequential HTTPS with session-owned
   callbacks, per-request ambiguity tracking and bounded server I/O. Preserve
   `m_work`, payloads and economic retry. A/B the same isolated read and trade
   cases, cold and warm, with an unchanged control. No push channel yet.
2. **Two-lane ownership:** asynchronous transport with serialized state owner,
   strict generations, bounded foreground/passive jobs and shutdown. Prove
   a deliberately slow refresh cannot occupy the trade's connection slot.
3. **Live evidence:** first prefer a negotiated deferred watch that returns a
   bounded existing event-page-shaped JSON result on change or expiry. Register
   a waiter, release the server worker, and complete it from the I/O owner;
   never sleep inside the current synchronous handler. This is event-driven
   long-polling on a warm connection, not an implemented continuous stream.
   Existing proofs are initially obtained through unchanged verification reads.
   Optional canonical-proof delivery and continuous framing are separate later
   refinements, with explicit versioning and byte bounds. Preserve replay/gap
   handling and old endpoint fallback without claiming unsupported streaming.

Each stage stops for review of its actual delta before the next. No one giant
rewrite; no move to consensus changes to explain unmeasured client delays.

Required regressions:

- Multiple requests share one verified connection; old close-after-response
  peers still work; stale/half-closed sockets, partial writes, lost responses,
  shutdown and trust/pin/hostname failures stay conservative.
- Warm second submission loses its response: original bytes, ActionId,
  sequence, uncertainty and no-resubmit markers survive restart unchanged.
- Framing ambiguity, oversized body, slow sender/reader, handshake flood,
  idle clients and repeated reconnect do not exhaust workers or bypass limits.
  The known oversized-rejection/write/close assertions are not weakened.
- Slow passive reads plus queued review/status, wallet/market/selection change,
  duplicate/out-of-order callbacks and hidden-panel Quit preserve attribution.
- Subscribe/certify race, duplicate event, expired cursor, restart, stale and
  forged proof, newer B3 tip/reorg, missing evidence and server failover do not
  upgrade hints into credits, reset authority or create replacement actions.
- Visible and hidden native Qt Quit capture actual clean exit, then same-wallet
  reopen checks original instructions. Offscreen tests are not this check.

## 7. Measurements and approval boundary

Use bounded in-memory tracing enabled after setup, no synchronous per-event
log writes and no added polling. Record original ActionId across client queue,
work ownership, transport enqueue/TLS/write, server queue/handler/market-lock,
execution/agreement/durable append, result availability, client verification,
client durable save and display. Count connection reuse and TLS handshakes.
Attribute waits to the occupying request/lock or actual wake-up predicate.

Measure one matched fill and an eight-request burst with known liquidity;
separate no-fill accepted orders. Preserve identical source/configuration and
load between instrumented and uninstrumented controls. Report cold vs warm,
all-local vs remote-client, refresh on/off and healthy vs fault runs separately.
Use same-process monotonic deltas; do not subtract unsynchronized host clocks
or add overlapping durations. Count failures and initial retries, not only
successful requests. Small pilots do not establish a tail-latency SLA.

First success criteria are removed repeat handshakes, unchanged verification
and durability, bounded resources, clean lifecycle and attributed latency.
No 200 ms guarantee, 100-user capacity claim, stable-asset fee implementation,
V2 native integration or mainnet hard-fork activation follows from this work.
Keeping the existing signed/validity rules makes this client/API design a
transport upgrade, not by itself a consensus hard fork. FMN2 remains unchanged.

## References

- [Client priority baseline](../flowmesh-client-request-priority.md).
- [HTTPS source](../../src/node/flowmesh_https.cpp) and
  [public options](../../src/node/flowmesh_https.h).
- [Client receipt, outbox and snapshot logic](../../src/node/flowmesh_client.cpp).
- [Existing evidence and event bounds](../../src/flowmesh/client_evidence.h).
- [HTTP/1.1 connection persistence and framing, RFC 9112 §9.3](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.3).
- [HTTP/1.1 concurrency/head-of-line considerations, RFC 9112 §9.4](https://www.rfc-editor.org/rfc/rfc9112.html#section-9.4).
- [TLS 1.3 early-data replay limitations, RFC 8446 §8](https://www.rfc-editor.org/rfc/rfc8446.html#section-8).

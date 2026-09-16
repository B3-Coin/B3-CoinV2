# FlowMesh live delivery — second uncommitted integration stage

## Scope and qualification status

This stage targets completion and qualification of local live-message delivery.
It does not implement a distinct remote trading backend, an engine-off Qt
trading client, hosting commissions, committee changes, or validator removal.
The legacy default remains unchanged. Changes remain uncommitted for review.

**The bounded same-machine TCP qualification passed on 2026-09-11.**
Compiling a component, admitting a queue item, and writing a socket are not
evidence of a completed trade or of application on another replica.

The independent transport is FMN2 authenticated **plaintext TCP**, with separate
critical, action and bulk connections. FMN1 sessions are rejected explicitly;
independent-network operators need matching FMN2 software. Inner V1 FlowMesh
messages, signing digests, quorum, seat membership, certificates, checkpoints,
execution and B3 settlement validation are unchanged. This is not QUIC,
encryption, WAN performance, or a completed remote-client protocol.

## Outgoing critical-message ownership

1. The runtime constructs proposals, attestations and certificates through the
   existing execution/verification paths. Existing candidate persistence,
   persist-before-sign and persist-before-publication remain authoritative.
2. `FlowMeshRuntime::RelayMessage` retains exact critical wire objects under
   bounded local policy and supplies a process-local delivery identity. That
   identity is not serialized into the signed object.
3. `FlowMeshService::Impl::Relay` returns an explicit result. A stopped service
   or unreconciled B3 tip produces a refusal/retry reason, not a void return.
   Directed IDs retain the original transport: nonnegative B3 peer IDs and
   independent IDs at or below -2 do not overlap.
4. `FlowMeshNetService::Relay` synchronously admits or refuses each eligible
   peer's reserved traffic-class queue. There is no hidden intermediate outbox
   whose success can conceal later fanout refusal. Peer/global byte and item
   limits produce distinct reasons.
5. The socket worker drains those queues with critical-first bounded passes.
   Completion feedback names local socket-write completion, disconnect, stop
   or cancellation. Feedback runs outside transport locks and has its own
   bounded runtime queue. Refused feedback is counted; an explicit completion
   deadline prevents indefinite unobserved outstanding work.
6. The runtime rechecks object relevance before retrying. A new local attempt
   ID prevents late feedback from completing a replacement attempt. Retries
   preserve the signed object; obsolete work is cancelled without rewinding
   any signing journal. A completed socket write is not a terminal proof of
   remote delivery, so semantically current critical work remains retryable.

Actions, discovery and catch-up keep their existing bounded recovery owners.
Their callback outcomes are observations, not new signatures or an alternative
execution path. Historical certificates are recovered through verified durable
history rather than an unbounded outgoing archive.

Critical retention is bounded to 8,192 objects / 64 MiB globally and 16 MiB per
market. Each attempt tracks at most 64 peers. Relevant objects are paced at a
one-second repeat interval; missing local completion feedback expires after
five seconds. Both caller-triggered and periodic retries use the same relevance
gate. The worker scans at most 64 retained objects per tick with a rotating
cursor. Bounded regeneration from checked candidate/vote caches and the durable
head covers retention refusal; it does not create new signatures.

Permanent halts and local disarming retire outgoing signing traffic (not the
signing journal). Certificates do not require an armed local key. Transiently
paused retained objects expire after 60 seconds, checked by the bounded worker
scan. Once the chain gate reopens, exact reconstruction from checked sources
remains possible. A paused market cannot reserve its outgoing quota forever.

## Receive path and reconciliation

An independent frame must pass the FMN2 session, counter, size and transport
signature checks before service admission. That is transport authentication,
not FN eligibility or application verification.

If service admission refuses during reconciliation or because a runtime queue
is full, the connection retains the exact received object under the charged
receive budget and retries with bounded backoff. Retry expiry disconnects and
reports the reason; it does not acknowledge receipt or silently reset signing
state. Critical, action and bulk have separate connections and memory budgets,
so a retry-blocked action cannot hold a later vote behind it in one byte stream.

The additional runtime deferral handles reconciliation beginning *after* queue
admission. Deferred critical objects still require the ordinary anchor,
signature, membership and execution checks after the gate reopens. Capacity or
expiry must be observable and cannot be counted as successful verification.
This secondary queue is capped at 128 objects / 16 MiB globally, 16 objects per
market and eight per peer, with a 60-second expiry. A local reconciliation
generation also detects a gate that closes and reopens during handling. These
tokens are local observations, not consensus fields.

Legacy B3 carriage has no new remote receipt ACK or socket-completion adapter.
Its local queue results are labelled `legacy_queued_without_socket_completion`;
ingress refusal is logged explicitly. The independent receive-retention and
three-channel isolation guarantees must not be attributed to legacy carriage.
Current runtime critical retries help legacy delivery too, but an old peer's
failure reporting cannot be retrofitted by a local queue counter.

## Scheduling and limits

Independent per-peer send reservations are 8 MiB critical, 2 MiB action and
8 MiB bulk, with 1,024 / 512 / 128 item limits. Global class reservations are
32 / 8 / 24 MiB with 8,192 / 4,096 / 1,024 item limits; receive memory is also
class-separated at 32 / 8 / 24 MiB. Each worker pass allows at
most 512 KiB / 64 KiB / 128 KiB socket progress and 64 / 8 / 2 frame or retry
operations for critical / action / bulk, with rotated peer ordering.

Service-admission retries back off from 100 ms to one second and expire after
30 seconds. A partial incoming frame has a 30-second byte-progress idle timeout
and a separate five-minute absolute deadline. The runtime's existing five-second
catch-up request deadline remains separate; a slowly delivered transport frame
is not a guarantee that its catch-up request is still current.

Catch-up now adapts requested entry counts after timeouts: 64 → 32 → 16 → 8 →
1. The five-second request window, 15-second failure cooldown, 4 MiB reply cap
and all certificate/application checks remain unchanged. Only a full requested
page that is fully verified and applied within 2.5 seconds can double the count
(capped at 64). Slower successful pages keep their working count. Up to 256
peer/market profiles are retained; eviction excludes pending requests, and peer
removal/stop clears their profiles. Timeout, unmatched/expired/mismatched reply,
invalid framing/bounds and partial-application events record sizes and elapsed
time rather than implying a bad signature or successful catch-up.

This does **not** solve a single legal certificate whose transfer takes five
seconds or longer. There is no progress-based deadline extension here. V1 also
has no request nonce: an older reply that matches a renewed request must pass
that current request's bounds and all existing proof checks. An oversized old
reply can still consume that request under the existing rejection policy.

This is still one socket worker. A single frame's crypto/decode operation or
sink call is not CPU-preemptible. The maximum observed per-class pass work,
bytes and operations are instrumented. A bounded disconnect cleanup may also
drain queued work. These limitations must accompany latency results; separate
sockets do not establish complete CPU isolation.
Runtime retry rotation preserves budget-denied objects across changed attempt
IDs. The two-market regression checks that later owned votes are not starved,
but a large backlog can still require several bounded scans. No per-market
latency or arbitrary-load throughput guarantee is established.

## Observation semantics

`getflowmeshnetworkinfo` exposes class-local admission/refusal, currently
queued bytes/items, local writes, received transport-authenticated frames,
ingress retries, feedback refusal and bounded scheduling observations.

`getflowmeshdeliveryinfo [market_id]` is walletless, read-only runtime
instrumentation: created/admitted/refused/retried messages, verification,
certificate formation, durable local application and catch-up observations.
Each market includes a bounded recent event list with local monotonic times,
object identity and explicit reasons. It is not a complete persistent audit
journal. Counters reset on process restart; node-global event-queue overflow is
repeated in market rows, not a per-market sum.
`created` counts outgoing runtime objects, not newly generated signatures.
Reconstructed exact objects and their regeneration events must not be read as
new signing decisions. Runtime `socket_written` counts processed completion
feedback; transport totals count local socket completion itself. Cancellation,
replacement or peer removal can retire feedback before its runtime observation;
the recorded reason must not imply that bytes were never written.

An applied catch-up page with spare tail capacity is a scheduling observation,
not proof of the network tip. Catch-up to a fixed authenticated target,
operator arming, signing eligibility and actual emitted signatures are distinct.

## Isolated qualification design

`feature_flowmesh_independent.py` uses four separate headless regtest daemons,
four isolated wallets/seat keys and independent TCP sessions. B3 connections
remain available for blocks, but B3-carried FlowMesh is disabled and its message
byte counters must remain zero.

The inherited production fixture establishes real seats, asset deposits,
matching and settlement without weakening its existing rules. The additional
workload submits genuine bids/cancellations and a matching trade while B3
continues advancing. It then repeats with a gracefully disconnected validator,
a backlog, a held bulk channel and a 2 KiB/s-per-direction bulk channel.
No production wallet, existing key, journal reset or quorum exception is used.

For every measured action, the harness records RPC admission separately from
the client's first observation of a durable certificate. Every replica must
show the exact fixed target hash or a durable descendant containing that
ancestor. A greater sequence alone is insufficient. Catch-up and explicit
rearming readiness are measured separately. These are trusted local RPC
observation upper bounds, not a remote trading protocol or network ACK timing.

Replica polling starts after the client's certified-state observation. The
report's `replication_tail_ms` is submission-to-last-replica-observation time,
not extra latency after the client result. Catch-up timing starts at restart
and includes the intentional hold and intervening live workload; its fixed
target is selected after that workload, so this is not pure transfer latency.
The 2 KiB/s throttle applies to each affected connection direction, not the
recovering validator's aggregate bandwidth. The trade checks show B3 advancing
between submission and matching, not necessarily after bid certification.

## Execution results

Final four-process qualification passed. Completed checks:

- Apple Silicon/macOS headless Release build of `b3coind`, `b3coin-cli` and
  `test_bitcoin` passed. No Qt build or live wallet was replaced. The existing
  duplicate-static-library warning remains in the test link.
- 94 selected cases / 124,581 assertions passed in 9.676 seconds (Boost test
  duration). This includes all 34 runtime cases, all 17 socket transport cases,
  service/reconciliation/startup cases, frozen transport vectors, checkpoint
  codecs, FN binding and fee allocation. It is not the complete 1,550-case suite.
- The regression run caught suppressed caller forwarding, mutable-attempt-ID
  retry starvation and relevance/lifecycle mismatches. These were repaired;
  exact-byte, permanent-lock, quorum and durable-state assertions were retained.
- Two preliminary four-node trials stopped on explicit pre-admission RPC
  refusals during B3 reconciliation. The harness now records and retries only
  those known pre-admission refusals with the same explicit account sequence,
  within 30 seconds. Unknown outcomes and persistent pauses still fail. Neither
  preliminary trial is counted as successful end-to-end qualification.
- The third trial certified 30 actions with advancing B3 blocks, including
  genuine trades while a peer's bulk channel was held or throttled. Its slow
  replica failed the 120-second fixed-target catch-up check at 2 KiB/s. The
  transport received bulk frames without timeouts or ingress drops; after the
  throttle cleared, the replica applied the backlog. The five-second request
  window versus large response pages was identified as a remaining delivery
  limitation. This trial is not counted as qualified.
- The final adaptive-page build passed both four-node fault cycles, including
  catch-up while throttling remained in effect, explicit rearming and subsequent
  four-replica action checks. B3-carried FlowMesh message counters stayed zero.
- Final walletless network-policy/RPC smoke passed in 4.972 seconds from
  framework start to its success message. It checks independent channels and
  transport identity, legacy isolation, occupied-port failure without stopping
  B3, invalid settings and walletless diagnostics.

### Four-node measurements

The complete fixture and qualification ran from 15:12:10.668 to 15:16:16.039
UTC. The measured additional workload took 146.063 seconds: 54 certified
actions, seven matching trades (14 order actions), 20 bid/cancel pairs and
203 connected B3 blocks. The inherited fixture separately checked deposits,
custody sweeps, an exact-fee trade, withdrawals, graceful restart and clean
reindex. No power-loss/crash test is claimed.

| Observation from action submission | p50 | p95 | p99 / maximum |
| --- | ---: | ---: | ---: |
| Client's first observed durable certification, 54 actions | 232.026 ms | 2,139.608 ms | p99 2,323.016 ms |
| Replica 0, 54 live observations | 237.762 ms | 2,140.188 ms | max 2,333.409 ms |
| Replica 1, 54 live observations | 238.459 ms | 2,140.674 ms | max 2,334.096 ms |
| Replica 2, 54 live observations | 239.274 ms | 2,141.190 ms | max 2,334.775 ms |
| Replica 3, 14 live observations | 222.489 ms | 322.394 ms | max 322.394 ms |

Replica 3's other 40 actions were verified through two exact-target catch-ups,
not falsely reported as live observations while it was offline.

| Recovering replica 3 | Cycle 1 | Cycle 2 |
| --- | ---: | ---: |
| Restart → authenticated three-channel mesh | 646.365 ms | 584.963 ms |
| Restart → fixed target durably observed (includes hold/workload) | 62,640.480 ms | 64,781.952 ms |
| Explicit arm → eligible observation | 13.472 ms | 13.455 ms |
| Target sequence / distance | 36 / 20 entries | 58 / 20 entries |
| Actions certified by the live quorum while bulk was held | 4 | 4 |

Each held and slow-bulk phase included a real matched trade and cancellation.
The slow limit was 2 KiB/s **per affected connection direction**. No journal
reset, replacement key, quorum exception or automatic rearming was used.
Eligibility is not a claim that a particular subsequent signing share was
observed. Four explicit pre-admission paused responses were recorded and safely
retried; their wait is included in the latency measurements.

Every replica reached final certified sequence 60, with exact head
`f21a7142bb633325efd264927744f31b7f831e6d47c9ecc768ef9b1c141242ee`
and state root
`8fbb7d2434c0f81c15f3258ee39fd646316495098ee49a635ed6bb1ec01b509f`.
The harness checks exact heads or exact durable ancestors, never height alone.

Final runtime snapshots recorded zero completion-feedback overflows and zero
deferred-ingress capacity/expiry refusals. B3 reconciliation did cause critical
deferrals (19 / 10 / 16 / 2 in the respective final process lifetimes). Replica
3's counters reset on each restart; its final lifetime recorded 22 durable
applications, 11 catch-up timeouts, nine refused replies and three partial pages
before eventual convergence. These failed attempts are not counted as completed
catch-up. One current 782-byte certificate remained retryable on each node at
the final observation; a socket write alone is not a terminal receipt proof.

All fixture processes stopped. State and logs are retained for review; no live
wallet, key or data directory was opened, and no changes were committed or
pushed. HEAD remains `59057cb6c85d32129e1e38c3dd5b67e7c8b36e02` on
`release/v1.1.5`, alongside the pre-existing unrelated uncommitted work.

Final daemon SHA-256:
`e7093b22fce00c72c12a148cd732e85491c6a93897193a19b8f143e80817c035`.

Commands and logs, relative to this worktree:

```text
cmake --build build-fmnet-stage1 --target b3coind b3coin-cli test_bitcoin -j 4
build-fmnet-stage1/bin/test_bitcoin --run_test=flowmesh_runtime_tests,flowmesh_net_tests,flowmesh_service_tests,flowmesh_transport_policy_tests,flowmesh_service_startup_tests,flowmesh_transport_compat_tests,flowmesh_checkpoint_codec_tests,flowmesh_seat_binding_tests,flowmesh_fee_allocation_tests --log_level=test_suite --report_level=detailed --color_output=no
python3 test/functional/feature_flowmesh_independent.py --configfile=build-fmnet-stage1/test/config.ini --nocleanup
python3 test/functional/feature_flowmesh_network_policy.py --configfile=build-fmnet-stage1/test/config.ini --nocleanup
```

- `build-fmnet-stage1/delivery-build7.log`
- `build-fmnet-stage1/delivery-final-focused-tests7.log`
- `build-fmnet-stage1/delivery-four-validator-run3.log` (failed slow catch-up)
- `build-fmnet-stage1/delivery-four-validator-run4.log` (passed)
- `build-fmnet-stage1/delivery-final-network-policy.log` (passed)

The full per-action and per-node JSON evidence is retained at:

```text
Private retained generated-fixture evidence: fmnet-qualification.json
```

### Still outside this qualification

This does not establish QUIC, transport encryption, WAN latency, arbitrary-load
throughput, crash recovery, liveness after loss of the existing quorum, or
automatic recovery from permanent signing conflicts. Count adaptation cannot
make one oversized certificate fit the unchanged five-second reply window.
Legacy B3 carriage does not acquire the independent transport's receive
retention/three-channel isolation guarantees. The next distinct remote trading
backend and Qt client path remain unimplemented; the engine must not become
default-off until an engine-off ordinary client can actually trade.

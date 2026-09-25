# Native queue attribution — 2026-09-25

## Outcome

200 ms is **not achieved**. This is native V1 with its preagreement layer and
separate V1 final certificate, not native integration of the V2 models or a
stable-coordinator implementation. No trading, consensus, retry, quorum or
durability rule was changed. No performance repair is claimed.

The corrected memory-only pair completed nine genuine fills each, verified
buyer/maker balances and exact signed bytes, advanced B3 in both windows, and
captured clean exit status 0 for all five processes. No capture records dropped.

| Case | Capture off | Memory capture on |
|---|---:|---:|
| Single fill: original submission to client-verified inclusion | 445.572 ms | 433.402 ms |
| Eight simultaneous fills: individual inclusion latencies | 1,243.518–3,884.691 ms | 844.188–3,625.243 ms |
| Single fill: authenticated account-balance observation | 472.635 ms | 480.579 ms |

One pair and one isolated sample per arm cannot establish a median, p95,
zero instrumentation overhead or a speed improvement. Do not pool this with
the earlier 40-action campaign. Controls use the same instrumented executable
with capture disabled, not a separately optimized uninstrumented binary.

## Frozen sources and environment

- Branch: `flowmeshV2-dev`; previous source `a463be47649a9afa52052db2e7913b79fef1c2b8`.
- Compiled C++ source: `d48da5a59edf744c700dfb22124940d8721846ca`.
- Measured daemon SHA256:
  `0661d04e162a45b63dce901ad8b59497308b81a4d94da8eb9458fc68b07d2306`.
- Corrected harness: `bfff11d1f932171b005cf460373db77d4627ba7c`.
  The intervening commits only correct fixture capture/accounting; C++ is
  unchanged. Both final runs use this harness and the same daemon.
- Apple M4 Max, 16 CPU cores, 128 GiB RAM; macOS 26.5.2 / 25F84.
  Shared workstation/storage, not a controlled dedicated host or WAN.
- Optimized RelWithDebInfo build; four separate generated regtest operators,
  real native BLS and synchronous LevelDB writes, one engine-off client,
  two markets / four buyers / funded standing asks. Independent authenticated
  plaintext TCP and loopback HTTPS via the existing test relay.
- Seed 24092026, port seed 92401. No intentional transport faults.
  Regtest mock-time B3 advances: control 227→229 and 231→238; capture
  226→228 and 229→235 (all four operator replicas).

Commands, capture bounds and clock caveats:
[protocol](../test/functional/flowmesh_native_queue_protocol.md).

## What the trace establishes

**Busy worker, not simply a sleeping validator.** For burst request 6, a
specific ACTION frame spent **720.176 ms** in node1's queue (919.066→1,639.242 ms
after submission). **719.220 ms** is covered by that worker's non-overlapping
processing intervals: 603.212 ms handling agreement messages, 90.847 ms ticks,
24.726 ms attestations, 0.250 ms actions, 0.185 ms certificates. These are
wall-clock processing intervals, not CPU-only measurements. Nested signing,
verification, persistence and trace costs are included; do not add them again.

**Queueing makes a transport envelope stale.** This frame carried sequence 8.
Node1 durably completed sequence 8 at 1,582.1 ms before dequeuing it at
1,639.2 ms. The remote-header equality guard in
[HandleAction](../src/node/flowmesh_runtime.cpp) therefore rejects that old
envelope; it does not erase the original signed instruction. A refreshed
sequence-9 envelope was created by node0 at 1,990.1 ms and admitted remotely
around 2,000 ms. Normal retries preserved the same action. Request 6 was
selected later at 3,222.0 ms and client-certified at 3,625.2 ms.

**Reconciliation also participates.** Request 7's sequence-9 traffic includes
explicit `market_unavailable_or_reconciling` refusals and retained
`b3_reconciling_after_admission` messages. The eventual certificate does not
prove those intervals were network loss or the two-second round timeout.

**Client serialization is real.** Market refresh, submission and status
calls hold [m_work](../src/node/flowmesh_client.cpp) across HTTPS/verification
and synchronous outbox saves. Across this captured workload the longest
Submit span was 264.089 ms and Market span 251.095 ms, including lock wait.
HTTPS and server API spans peaked at 118.875 / 115.409 ms respectively; these
are overlapping intervals, not extra durations to sum into a latency budget.

**Persistence has variable cost, but is not proved to be the sole cause.**
An earlier body-file diagnostic run recorded 1,562.5 ms inside a synchronous
agreement database write and 1,496.3 ms inside an outbox write. These large
pauses did not repeat in the corrected memory-only run. Its node0 agreement
write maximum was 31.707 ms; client outbox-save maximum was 36.932 ms.
[Persist](../src/node/flowmesh_agreement.cpp) brackets
[WriteBatch](../src/dbwrapper.cpp), not the operating system's sync syscall
alone. [LevelDB's macOS path](../src/leveldb/util/env_posix.cc) uses
F_FULLFSYNC, but disk service time versus internal queueing/descheduling was
not separately measured. The earlier observation remains evidence, not a
diagnosis generalized across runs.

## One timeline per measured request

All columns are milliseconds from that request's original submission, not
sequential durations. “All applied” is the latest observed durable-apply event
on four replicas, **not additional replication delay**. Action identities are
the tuple (exact MarketId, ActionId); ActionId alone can repeat across markets.

| Request | First pool admission | Selected | First final certificate | All applied | Client inclusion | Account proof |
|---|---:|---:|---:|---:|---:|---:|
| 0 isolated | 59.8 | 69.6 | 383.9 | 460.2 | 433.4 | 480.6 |
| 1 burst | 33.3 | 51.9 | 677.0 | 778.5 | 844.2 | 1299.0 |
| 2 burst | 888.5 | 902.0 | 1475.5 | 1994.4 | 1857.3 | 1882.0 |
| 3 burst | 1573.5 | 2491.8 | 2840.2 | 2927.0 | 3043.0 | 3087.9 |
| 4 burst | 546.4 | 2035.2 | 2381.1 | 2442.3 | 2636.9 | 2764.8 |
| 5 burst | 851.5 | 905.5 | 1431.2 | 1582.1 | 1749.0 | 1775.5 |
| 6 burst | 910.9 | 3222.0 | 3496.3 | 3617.7 | 3625.2 | 3647.2 |
| 7 burst | 546.1 | 1634.9 | 3085.5 | 3123.7 | 3117.9 | 3147.9 |
| 8 burst | 56.0 | 59.6 | 812.3 | 869.4 | 923.0 | 1367.3 |

For request 0, proposal publication callback began at 92.28 ms; first PREPARE
publication callbacks on the four nodes span 118.28–190.81 ms, COMMIT
252.60–291.04 ms, durable decisions complete 307.31–345.40 ms. The subsequent
V1 certificate first formed at 383.916 ms. Publication callbacks are not
proof of peer receipt. Raw per-request records retain phase, thread, candidate,
wire hash and parent-span correlation rather than combining campaign medians.

## Qualification and retained failures

- 98 affected native cases passed on the committed-source build (agreement,
  runtime, client poll scheduler and new timing collector); 22 Python metric
  and trace-analysis checks passed. The collector additionally passed its
  count/byte bounds, disabled-state, nesting and frozen-read check separately.
- All 18 requests in the final pair passed; exact-byte, maker-conservation,
  engine-off, independent transport and clean-child-exit assertions passed.
- Memory events by node: 29,448 / 30,279 / 30,301 / 31,557 / 2,131; zero drops
  and zero stream-limit markers. RPC observations: control 942, capture 878,
  zero drops. No native diagnostic event lines appeared in debug logs.
- The first capture completed its trades but failed an inherited assertion
  demanding zero diagnostic bytes. The fixture now distinguishes bounded
  memory bytes from enabled debug disk logging; the no-logging/no-drop
  assertions remain strict. That failed run is preserved and not called green.
- Initial compiler failure (RPCExamples construction) and Python invocation
  error are preserved. A bare version probe attempted default settings access
  and the sandbox denied the write; it was not retried with elevated access.
  Build identity was established from generated build metadata and hashes.
  All actual test launches used explicit disposable datadirs.

Private evidence is retained in the local operation directory named
`native-queue-attribution-20260925.tzXlhp`, outside the repository.
It includes complete original reports, logs, per-node frozen captures,
`memory-timelines.json` and `summary-timelines.json`; generated wallets/keys
are not publication material. Final report SHA256:

- control: `38f8e710ef672e9c9e8d4052de41323050091c9ef943e48d46b48f23a75ee395`
- capture: `ec35ff9fc410df31466e318645f002e6b8815b6787722d46c31a7e4b8106ad35`

## Limits and next bounded action

Still unknown: exact client kernel socket-write time; OS sync vs LevelDB
queue/CPU time; scheduler descheduling; complete causal decomposition of every
pre-admission gap and every B3 reconciliation wake-up. HTTP dispatch is not a
socket write. A span crossing capture boundaries may be absent. Clock alignment
uses RPC brackets (140–148 microseconds wide), not an assumed shared epoch.
The collector accounts serialized bytes, not total heap use, and introduces
nonzero allocation/locking overhead. No arbitrary-load, WAN, power-loss,
production-V2, stable-leader or hosting-commission qualification is claimed.

No safe one-line scheduling defect was demonstrated. Do not shorten retries,
relax stale remote-header checks or remove durability barriers merely to pass.
**Recommended next action:** isolate the measured queued-ACTION/head-advance
case in a focused native regression, then evaluate a bounded, current-head
tail-forwarding wake-up after durable commitment. Require exact signed-byte
preservation, terminal inclusion checks, unchanged admission/quorum rules and
non-starvation of votes before considering a repair. Keep a next-stage proposal
separate from this completed measurement.

Changes are local diagnostic/tooling commits only. Independent implementation
review remains pending. No push, PR, live signer operation or deployment.

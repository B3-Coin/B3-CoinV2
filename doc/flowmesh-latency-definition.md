# FlowMesh latency definition (flowmesh-latency-v1)

One definition for every FlowMesh latency number, so that Mac, Linux-VPS and
WAN results can be compared honestly. A number that does not follow this
definition, or lacks the metadata below, is not a FlowMesh latency result.

The working gate and the stage-by-stage measurement plan stay in
[flowmesh-performance-baseline-plan.md](flowmesh-performance-baseline-plan.md).
This document fixes what the headline number means and what every report has
to carry.

## M1: client-certified latency (the headline number)

| | |
|---|---|
| Start, t0 | Client-host monotonic time immediately before the order RPC (`submitflowmeshorder` or `cancelflowmeshorder`) is invoked. It is taken once per instruction. An explicit refusal before admission that retained nothing ("FlowMesh service is reconciling the B3 tip", "FlowMesh market is paused (at least four active seats are required)") does not reset it. |
| End, t1 | Client-host monotonic time when a `getflowmeshactionstatus` read, waited or not, or the order RPC itself when it was given `wait_ms` ([waited submission](flowmesh-waited-submission.md)), first returns `receipt_state` `certified_inclusion` with `certificate_verified` true for the same ActionId. The client has then verified the BLS certificate under its local B3 authority and anchor. |
| M1 | t1 - t0. This is the harness's `client_certified_ms`: `summary.client_certified` in `feature_flowmesh_persistent_trades.py`, `client_certified_ms` in `feature_flowmesh_latency.py`. |

M1 includes the wallet's signing preflight (a market refresh, or joining one
in flight), signing, the client outbox save, the HTTPS submit, admission,
relay, proposal, attestation and certificate, the durable append, the event
log, the status read or wait (or the submission's own wait), and client
verification.

M1 excludes the execution outcome and fill (M2), the B3 checkpoint, and Qt
rendering.

A retry of the same saved ActionId stays inside its sample. A sample that
does not reach verified inclusion within its 60 s deadline is a failure: it
stays in the denominator and is reported, never dropped.

**"From signing".** R7 of the Hyperliquid learnings asks for latency "from
signing". M1 starts before the order RPC, so it also contains the signing
preflight. M1 stays the gate metric because it is what the existing harness
and gate measure. A sign-complete-to-certified figure, if one is ever
reported, must be named M1s and must never be compared with M1.

## M2 and M3

- **M2, account state verified** (`account_state_verified_ms`): t0 to the
  first authenticated account state that reflects the order.
- **M3, initial response** (`initial_response_ms`): t0 to the order RPC's
  return.

## Statistics

- Report n, and the nearest-rank p50, p90, p95, p99 and maximum. The value at
  quantile q is the sample at rank ceil(n x q) (`distribution()` in
  `feature_flowmesh_latency.py`).
- Exclude warmup samples, and report them separately.
- p99 is not estimable from fewer than 100 samples. The harness reports
  `p99_estimable: false`; the value is shown but no p99 claim may be made.
- Qualification needs n >= 1,000, pooled from runs of identical
  configuration. Report each run and the pooled result.
- The working gate is unchanged: co-located, p50 <= 200 ms and p95 <= 600 ms.
  No WAN gate exists; setting one needs an owner ruling.

## Mandatory metadata

Every published number carries all of these:

| Item | Where it comes from |
|---|---|
| Source commit, binary SHA-256, build type | `provenance` (hashes of every process binary), `environment.source_commit`, the build's `CMAKE_BUILD_TYPE` |
| Host: CPU, cores, RAM, OS and kernel, storage | `environment` (CPU, cores, OS release); RAM and storage by hand |
| Durability barrier (fsync mode) | `environment.fsync_mode` (see below) |
| Topology and regions | `environment.client_region`, `environment.server_region`, `environment.topology` |
| Round-trip time | Measured by hand, see below |
| Transport arm and wait arms | `arm`, `status_wait`, `submit_wait` |
| Whether B3 advanced during each sample | `summary.b3_during_bid`, per-sample `b3` |
| Logging mode and load | The harness runs with `-debug=0` and a sequential load |

**fsync mode.** How the FlowMesh store's LevelDB makes each append durable:

| Label | Meaning |
|---|---|
| `full_fsync` | macOS `fcntl(F_FULLFSYNC)`: flushes the drive cache (build flag `HAVE_FULLFSYNC=1`). |
| `fdatasync` | Linux `fdatasync` (`HAVE_FDATASYNC=1`). |
| `fsync` | Plain `fsync` on a platform without either. |
| `fsync_without_drive_flush` | macOS build with `HAVE_FULLFSYNC=0`: `fsync` does not flush the drive cache. An experimental arm, never comparable with `full_fsync`. |

`--fsync-mode=auto` (the default) reads `HAVE_FULLFSYNC` and `HAVE_FDATASYNC`
from the configured build's `CMakeCache.txt`. Binaries from any other build
need an explicit `--fsync-mode`. A run whose barrier stays `unknown` is
reported with `performance_measurement_qualified: false`.

**Round-trip time.** Take the median of 20 TCP connect times from the client
host to the API endpoint the client uses. `curl`'s `time_connect` is one
network round trip. Space the calls at least 300 ms apart, because the public
`health` method allows four calls per second per client:

```
for i in $(seq 20); do
  curl -s -o /dev/null -w '%{time_connect} %{time_total}\n' --cacert ca.pem \
    -H 'Content-Type: application/json' -d '{"method":"health","params":{}}' \
    https://API_HOST:5650/flowmesh/v1
  sleep 0.3
done | sort -n | sed -n '10p;11p'
```

Report the median `time_connect` as the RTT. `time_total` is a cold request
that includes the TLS handshake; report it too, labelled as cold.

## Local procedure

```
test/functional/feature_flowmesh_persistent_trades.py \
  --configfile=build/test/config.ini \
  --transport-arm=warm --samples=250 --status-wait-ms=2000 \
  --fsync-mode=auto --client-region=co-located --server-region=co-located \
  --tmpdir=<fresh directory> --nocleanup --portseed=<unique>
```

- One run has at most 255 samples. Repeat it four times, each time with a fresh
  `--tmpdir` and a distinct `--portseed`, for n = 1,000.
- Pool the four runs' `samples[*].bid.client_certified_ms` and compute the
  nearest-rank statistics over the pooled list. Report each run as well.
- The report is `persistent-trades.json` in the run directory. `definition`
  names this document, and `environment` holds the metadata the harness can
  observe.
- The Linux arm (R7, E0) runs exactly the same commands on a Linux host, where
  auto gives `fdatasync`.
- Book-depth arms with 512 or 1,024 resting curves cannot be run with the
  current fixtures, which have no preload. Record them as "not executed".

## WAN procedure

- Run an engine-off client whose `-flowmeshendpoint` entries point at remote
  observer APIs, on regtest, testnet or a private network, and use the same
  RPC loop. Record both regions and the RTT.
- Never place mainnet orders to measure latency without explicit owner
  approval: they are real orders that pay fees.
- Do not compare a result with another venue's figure unless that venue
  publishes its definition and it is the same as M1. Hyperliquid's roughly
  0.2 s median is measured co-located, and its definition is not published.

## UI responsiveness (U1-U5, provisional)

M1 stops at the client's verified receipt; it never includes Qt. These
metrics cover the trading panel itself: how soon a certified head is on
screen, and what the GUI thread spends. They are **provisional targets** from
the 2026-09-30 UI responsiveness design and critique, for the owner to accept
or amend. Same rules as above: steady clock only, nearest-rank statistics,
n reported, p99 only from n >= 100, and the metadata of the harness report.

"Shown" (the end event of U1 and U5) is when the order book installs rows
for a head at least as new as the one measured, or, when that install
changed pixels, the paint that follows it. A head whose changes lie below
the visible rows paints nothing, so "first paint" alone would overstate U1.
"Quiet" means an inter-head gap of at least one second: two refills of the
500 ms live-update pacing (the watch's burst of two), which today applies to
the local engine and a remote endpoint alike; if the local pacing changes,
"quiet" for U1-local is two of its refills. Anything faster is "busy". The
harness's quiet run holds about 24 heads (30 s at one per 1.2 s), so it
estimates p95; the quiet p99 target needs a run of at least 100 heads and is
not gated.

| Metric | Measures | Provisional target |
|---|---|---|
| U1-local | Local engine: head committed -> the book shows it | Quiet: p95 <= 50 ms, p99 <= 100 ms. Busy at 5 heads/s: p95 <= 600 ms at the shipped two local reads a second (one 500 ms refill plus processing); p95 <= 150 ms needs ten a second (C7, held back, see below). 20 heads/s is a stress case, reported only. |
| U1-qt | Any backend: verified reply returned -> the book shows it | p95 <= 30 ms |
| U1-remote | Remote endpoint head -> the book shows it | Reported, not gated (a WAN gate needs an owner ruling); unchanged by this round: set by the endpoint's live updates (off by default, so a 0.5 s poll) and the per-head verified state download (see "Remote endpoint" below) |
| U2 | GUI stall at 20 applied updates/s with the pointer on the chart | Stall p99 <= 16 ms, max <= 50 ms; update handler p95 <= 8 ms |
| U3 | Chart hover, wheel zoom and drag pan paint, 1320 x 940 | p95 <= 8 ms |
| U4 | The 500 ms housekeeping tick with unchanged data | <= 2 ms and no book or chart paint (the marketable limit is included) |
| U4b | An unchanged reply (quiet watch timeout, same-head poll), including the first one after each new head | <= 2 ms, no view rebuild and no book or chart paint (remote and local) |
| U5 | Market switch -> the new market's book is shown | <= 50 ms (local engine: a fresh verified read) |

**Harness.** `test_b3_flowmeshperf-qt` (QT_QPA_PLATFORM=minimal) runs these
against a synthetic in-process market: 57 limit orders a side from 114
accounts, a four-price ladder a side, the viewer's own bid, 50 book levels a
side, 256 held ticks with observed times and 300 one-minute candles. No
socket, signing or submission is involved. By default only 10x a target
fails (a smoke check), and no check depends on the machine keeping up:
pacing floors allow 100-200 ms of thread-start and timer jitter, and
throughput, "every head shown" and frame-rate checks are skipped, so CTest on
a loaded machine does not flake. `FLOWMESH_UI_PERF_STRICT=1` asserts the
targets and those timing checks (meant for a Release build on the owner's
Mac); `test_b3_flowmeshworkspace-qt` honours the same variable for its
measured live-update spacing and flash frame rate. `FLOWMESH_UI_PERF_REPORT=<absolute
path>` writes the JSON report with distributions and metadata (build type,
Qt, QPA, CPU, cores, OS, version; `FLOWMESH_UI_PERF_COMMIT` names the
commit). Strict and report runs use the longer sample windows.

```
FLOWMESH_UI_PERF_REPORT=$PWD/ui-perf.json FLOWMESH_UI_PERF_COMMIT=$(git rev-parse HEAD) \
  QT_QPA_PLATFORM=minimal build-qt/bin/test_b3_flowmeshperf-qt
```

The harness also checks two pacing rules: a remote endpoint's live reads
never exceed a burst of two and then one per 500 ms, and a remote endpoint
without live updates is polled on the 500 ms tick, so a slow link's reads
never start back to back (the remote client's work gate is not preemptive).

**Validator A/B for local reads.** `feature_flowmesh_latency.py
--latency-local-reader-hz=N [--latency-local-reader-busy] --latency-lock-capture`
serves a trading panel's local live updates on operator 0 during the measured
phase (as the panel's watch worker sends them, at up to N reads a second) and
records operator 0's market-mutex waits per holder: the FlowMesh runtime
worker (ProcessTick, ProcessMessage, ProcessDeliveryEvent; pooled as
`runtime_worker_wait_ms`, the tick alone as `runtime_tick_wait_ms`), client
calls (`client_calls_wait_ms`: submissions, status and snapshot reads) and
MarketData reads (the reader's and the fixture's own). Compare the worker's
tail per holder across arms: pooled, its thousands of near-zero message
waits hide a tick that waits behind a reader. The busy mode never waits
(every read is a full read): an upper bound of the reader's load at that
rate.

**Measured on this branch** (RelWithDebInfo, minimal QPA, the owner's Mac,
strict report runs; before this round -> after it, with local reads held at
two a second):

| Metric | Before | After |
|---|---|---|
| U1-local quiet | p95 12 ms | p95 18 ms (n = 24; within noise, target 50) |
| U1-local busy, 5 heads/s | p95 485 ms | p95 420 ms (two local reads a second; 15 ms was measured with ten) |
| U1-local stress, 20 heads/s | p95 470 ms | p95 507 ms (two a second; 74 ms with ten) |
| U1-qt | p95 11 ms | p95 12-17 ms |
| U2 handler (B3 view) / stall p99 | 1.2 ms / 4.6 ms | 1.4 ms / 4.2 ms |
| U3 hover, zoom, pan | p95 0.5 ms | p95 0.7-0.9 ms |
| U4 idle tick (marketable) | 0.01 (0.12) ms, no paint | same |
| U4b unchanged reply | 1.0 ms, **the book repainted every time** | 0.07 ms, no paint; the first one after a new head no longer rebuilds the views |
| U5 market switch (local) | p95 20 ms | p95 21 ms |

A strict run (`FLOWMESH_UI_PERF_STRICT=1`) passes every provisional target
on this Mac with this RelWithDebInfo build; a Release build is the intended
reference.

**Local reads at ten a second (C7): held back.** The provisional default of
2026-09-30 read the local engine up to ten times a second. Each local read
copies the book, the account and the store's history and ticks under the
node's market mutex, which the runtime worker's tick also takes. The A/B
(four operators, 2 + 40 bid/cancel pairs a run, n = 332-382 ticks a run;
two batches, the second with the per-holder report) cannot show that ten
reads a second is safe:

| Arm (operator 0) | Runs | ProcessTick wait p99 | ProcessTick wait max | M1 p50 |
|---|---|---|---|---|
| No reader (the fixture's own reads only) | 4 | 0.015-3.9 ms | 0.04-7.1 ms | 461-486 ms |
| 2/s, watching | 2 | 0.02 / 0.21 ms | 6.2 / 28.3 ms | 464 / 474 ms |
| 2/s, full reads | 2 | 0.12 / 0.11 ms | 6.8 / 57.2 ms | 466 / 467 ms |
| 10/s, watching | 2 | 1.05 / 0.12 ms | **1105.8** / 6.0 ms | 469 / 459 ms |
| 10/s, full reads (9.5-9.6 reads/s) | 2 | **10.2** / 0.22 ms | **25.7 / 200.9** ms | 483 / 489 ms |

The largest tick waits come with ten reads a second (one behind a 1.15 s
MarketData hold: the store's history and ticks are read under the mutex, so a
store stall becomes a mutex stall), and a single run's p99 moves by about 50x
between batches: at this size the A/B bounds
nothing, and the fixture has one market with few curves, so a real book's
copies hold the mutex longer. M1 p50 stays within run-to-run noise. The
pooled "other holders" p99 reported before (0.4-5.7 ms in every arm) mixed
about 2,500 near-zero message waits with client calls and hid the tick.

So the panel reads the local engine at the remote pacing (a burst of two,
then one per 500 ms; `LOCAL_ARM_REFILL_MS`), as it polled before this
round, and only while a review is open or nothing is in flight: never while
the user's own order is being sent. Ten local reads a second needs, first, a
node change that serves local reads outside the market mutex (a snapshot
published per certified head, and the store's history and ticks read
without it), then an A/B with at least 100 measured pairs a run on a
realistic book, for the owner's C7 ruling.

**Remote endpoint (the owner's Mac -> VPS).** U1-remote is unchanged by this
round. What the panel now does for a remote client: a verified reply with a
new head is applied and the book repainted at once (live update or poll);
same-head replies, including the first after each new head, rebuild no view
and paint nothing; cells change in place; with live updates the next watch
is armed on a precise timer, so on a busy market a remote endpoint's watch
reads now reach the full two a second (before, the tick held them 500-1000 ms
apart); a slow link's polls never start back to back. In the harness, the
remote pacing alone (no network, 5 heads/s, two reads a second) gives U1 p95
about 423 ms. The two levers that matter remotely, in order:

1. Enable `-flowmeshapiwatch` on the owner's own endpoints, after measuring
   the `state_bytes` each new head makes the client download on that market
   and the items of flowmesh-live-updates.md §6. Without it a remote trader
   polls every 0.5 s.
2. The owner's go/no-go on the verified incremental state apply
   (flowmesh-live-updates.md §4): today every new head costs the remote
   client an `updates` round trip, a whole-state `snapshot` download (up to
   8 MiB) with BLS and state-root verification and a durable high-water-mark
   save, all under its non-preemptive work gate. It needs its own design and
   review.

The remote book stays frozen during a review or while an order is sent.


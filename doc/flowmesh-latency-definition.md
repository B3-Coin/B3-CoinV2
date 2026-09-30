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
"Quiet" means an inter-head gap of at least one second (the watch's burst of
two times the refill); anything faster is "busy".

| Metric | Measures | Provisional target |
|---|---|---|
| U1-local | Local engine: head committed -> the book shows it | Quiet: p95 <= 50 ms, p99 <= 100 ms. Busy at 5 heads/s: p95 <= 150 ms with local reads up to 10/s (about 450-550 ms at two reads a second). 20 heads/s is a stress case, reported only. |
| U1-qt | Any backend: verified reply returned -> the book shows it | p95 <= 30 ms |
| U1-remote | Remote endpoint head -> the book shows it | Reported, not gated (a WAN gate needs an owner ruling); dominated by the per-head verified state download |
| U2 | GUI stall at 20 applied updates/s with the pointer on the chart | Stall p99 <= 16 ms, max <= 50 ms; update handler p95 <= 8 ms |
| U3 | Chart hover, wheel zoom and drag pan paint, 1320 x 940 | p95 <= 8 ms |
| U4 | The 500 ms housekeeping tick with unchanged data | <= 2 ms and no book or chart paint (the marketable limit is included) |
| U4b | An unchanged reply (quiet watch timeout, same-head poll) | <= 2 ms and no book or chart paint |
| U5 | Market switch -> the new market's book is shown | <= 50 ms (local engine: a fresh verified read) |

**Harness.** `test_b3_flowmeshperf-qt` (QT_QPA_PLATFORM=minimal) runs these
against a synthetic in-process market: 57 limit orders a side from 114
accounts, a four-price ladder a side, the viewer's own bid, 50 book levels a
side, 256 held ticks with observed times and 300 one-minute candles. No
socket, signing or submission is involved. By default only 10x a target
fails (a smoke check); `FLOWMESH_UI_PERF_STRICT=1` asserts the targets (meant
for a Release build on the owner's Mac); `FLOWMESH_UI_PERF_REPORT=<absolute
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
records operator 0's market-mutex waits, so M1 and the waits of the runtime's
other mutex holders can be compared at 0, 2 and 10 reads a second. The busy
mode never waits (every read is a full read): an upper bound of the reader's
load at that rate.

**Measured on this branch** (RelWithDebInfo, minimal QPA, the owner's Mac,
report runs; before -> after this round's responsiveness commits):

| Metric | Before | After |
|---|---|---|
| U1-local quiet | p95 12 ms | p95 17 ms (n = 24; within noise, target 50) |
| U1-local busy, 5 heads/s | p95 485 ms | p95 15 ms (ten local reads a second) |
| U1-local stress, 20 heads/s | p95 470 ms | p95 74 ms |
| U1-qt | p95 11 ms | p95 12-16 ms |
| U2 handler (B3 view) / stall p99 | 1.2 ms / 4.6 ms | 1.1 ms / 4.2 ms |
| U3 hover, zoom, pan | p95 0.5 ms | p95 0.6 ms |
| U4 idle tick (marketable) | 0.01 (0.12) ms, no paint | same |
| U4b unchanged reply | 1.0 ms, **the book repainted every time** | 0.05 ms, no paint |
| U5 market switch (local) | p95 20 ms | p95 20 ms |

A strict run (`FLOWMESH_UI_PERF_STRICT=1`) passes every provisional target
on this Mac with this RelWithDebInfo build; a Release build is the intended
reference.

The remote pacing without a network (5 heads/s, two reads a second) costs
U1 p95 about 415 ms; WAN U1-remote adds the link and the per-head verified
state download (reported only).

The validator A/B (four operators, 40 measured bid/cancel pairs a run):
M1 p50 461/481 ms with no reader, 464 ms (2/s) and 469 ms (10/s) watching,
466 ms (2/s) and 483 ms (10/s, 9.5 full reads a second) never waiting; the
other market-mutex holders' wait p99 stayed 0.4-5.7 ms in every arm. The
runs also showed waits of 1-3 s on the market mutex in arms with and
without a reader: MarketData reads the production store's history and
ticks under the market mutex, so during a disk-flush stall the runtime
tick can wait behind a reader that waits on the store mutex. That is a
node issue independent of the panel's read rate, reported separately.


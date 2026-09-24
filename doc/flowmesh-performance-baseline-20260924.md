# Current executable performance baseline — 2026-09-24

## Verdict and identity

**TARGET NOT MET. No sustainable rate meeting p50 <= 200 ms and p95 <=
600 ms was established.** These are preliminary, small-sample LOOPBACK
measurements, not V2 or WAN qualification. No production optimization was
made. The current executable also refused one post-certification balance
read during the two-market repeat; that failed run is preserved.

The compiled source is frozen at
`f3f2b86840df97c6666379c6c96d4fc26c786213` on `flowmeshV2-dev`.
Its pending header-repair review remains separate. Later local commits
change only benchmark code/documentation. They do not rebuild or change
the measured C++ implementation.

- Runtime: experimental **V1 pre-agreement**, explicitly enabled for fresh
  generated markets, followed by the existing V1 final BLS certificate.
  A preliminary agreement certificate is not the measured final result.
- Four independent validators, four seats, **3/4 quorum**; one ordinary
  `-enableflowmeshvalidator=0` client process hosting generated test wallets.
- Real BLS12-381/blst signatures and BIP340 signed actions, normal
  verification, synchronous safety journals and client outbox writes.
- B3-carried FlowMesh traffic disabled and checked; independent FMN2
  authenticated plaintext TCP. HTTPS verifies a generated test CA.
- B3 advances using the existing regtest staking/mock-clock fixture, not a
  frozen chain. The fixture's one-second block schedule is not mainnet.
- Service maintenance 250 ms; existing two-second view/round timeout;
  action wakeups and one-second recovery retries unchanged. One service
  worker; unchanged batch bounds (1,024 actions / 2 MiB). Retry scheduling,
  quorum, signing and persist-before-publication rules were not tuned.

The Python V2 agreement/accounting/storage models are **not** the runnable
exchange. Missing V2 production integration includes real authenticated
agreement wire/crypto, production storage/fencing and recovery, runtime and
transport integration, changing membership/PoS V2, authoritative bootstrap
and V1 obligation transition, authenticated custody/unchanged-bridge
compatibility, shared-ledger/client-proof integration, and futures
margin/oracle/liquidation. No V2 timing is claimed.

## Optimized build and host

Daemon SHA256:
`a2737f4c3ff5b999ec61faa381c6fc724017e2de40af84ce463f0ac0db4921f9`.
It reports `v1.1.5-flowmesh-test.2-f3f2b86840df`.

RelWithDebInfo (`-O2 -g`), AppleClang 17.0.0.17000013, GUI OFF,
wallet ON, tests ON, bench OFF, ccache OFF, IPC default ON; no sanitizers.
OpenSSL 3.6.3, libevent 2.1.13, system SQLite 3.51.0 runtime
(SDK header detected as 3.43.2). Targets: `b3coind`, `b3coin-cli`,
`b3coin-wallet`. No build was concurrent with a measured run.

Apple M4 Max MacBook Pro Mac16,5, 16 cores (12P/4E), 128 GiB RAM,
8 TB internal APPLE SSD AP8192Z/APFS; macOS 26.5.2 (25F84), arm64.
All five processes and the test proxies share this machine. The TLS relay
introduces two TLS legs. Each client request uses a new verified TLS
connection: no persistent-session result exists.

This was **not a dedicated idle host**. Background user applications were
left untouched. Preflight load averages were 3.69/2.98/2.51; diagnostic
sample 4.03/4.59/3.66; expanded02 sample 3.15/3.65/3.62. WindowServer and
browser processes were active. These are point samples, not continuous
CPU profiles or proof of controlled-load host performance.

## Measurement meanings

Primary: original client submission RPC call to client-verified final V1
certificate inclusion. Original start is preserved across exact signed
retries and explicit pre-admission waiting. Scheduled-offer-to-certificate
includes the separate account-worker queue. The scheduler does not wait
for responses. Admission/queued acknowledgement is not completion.

Inclusion alone has `outcome_verified=false`. Useful fills are checked by
authenticated account-state deltas against controlled counterparty orders;
decorative endpoint history is not treated as authenticated execution
evidence. Final maker conservation is a separate check, not implied by a
certificate. The two expanded runs did not reach/pass that final check.

Replica history observations are upper bounds on application, taken after
the normal durable publication path. They are not exact disk timestamps.
Submission-to-last-replica is not *additional* replication delay. Diagnostic
node-to-client comparisons use saved calibration bounds; ordinary stage
durations subtract only timestamps from the same process/trace segment.

First actual client-byte transmission, isolated signing/outbox preparation,
pure fsync, standalone client proof verification and Qt presentation are
**unknown/unmeasured**. The relay's request receipt is after incoming TLS
and parsing, not first transmission. B3 withdrawal settlement was not
measured. These gaps are not assigned zero time.

## Workload and rates

The plan was committed before execution in
[flowmesh-performance-baseline-plan.md](flowmesh-performance-baseline-plan.md).
One sequential pilot, separate diagnostic run, four taker wallets plus one
funded maker, two stable-test-token/B3 V1 markets, then fixed open-loop
0.5/1/2/4 actions/s, two 20-second windows per step, stopping escalation on
the declared gate/failure criteria. A separate eight-action burst is
retained. Each account/market has one unresolved sequence and a bounded
32-offer queue; all accounts share the existing client's serialized backend.

The two-market fixture starts with a 1,000-raw-unit maker ask per market,
four funded takers per market, and cycles matched bids, non-crossing bids
and cancellations. Quantity is one raw unit (0.01 token at two decimals),
crossing price 100,000,000 B3 atoms per raw unit (0.1 B3); non-crossing bids
use half that price. Encoded order actions are 279 bytes. These are small
real signed trades, not large-book/large-batch tests. Current V1 B3 fees and
market-local balances apply, not proposed V2 fees/shared balances/futures.

All quantiles below are empirical nearest-rank values in milliseconds.
With only 10–20 samples, p99 usually equals maximum; this is **not a tail
guarantee**. Failures remain in offered/failure-rate denominators and run
status. Latency quantiles use available observations; missing observations
are counted/reported separately, not assigned a fast completion time.
"Per-action checks" means the checks completed by that sample, not final
maker conservation or whole-run correctness. The sequential pilot checks
bid/cancel inclusion and replicas, not matched fills.

| Run / phase | Offered → certificate / per-action checks | p50 | p95 | p99 / max | >600 ms | Status |
|---|---:|---:|---:|---:|---:|---|
| pilot03, sequential bid/cancel | 20 → 20 / 20 | 394.990 | 519.336 | 882.695 | 1/20 (5%) | Correctness passed; median missed |
| expanded01, pilot | 12 → 12 / 12 | 460.300 | 3914.718 | 3914.718 | 3/12 (25%) | Run later failed harness join |
| expanded01, 0.5/s repeat 1 | 10 → 10 / 10 | 466.734 | 530.493 | 530.493 | 0/10 | Median missed |
| expanded01, 0.5/s repeat 2 | 10 → 10 / 10 | 499.016 | 1588.675 | 1588.675 | 2/10 (20%) | Both gates missed |
| expanded01, burst 8 | 8 → 8 / 8 | 1931.809 | 3111.564 | 3111.564 | 8/8 (100%) | Queue growth; run failed final join |
| expanded02, pilot | 12 → 12 / 12 | 487.020 | 1952.316 | 1952.316 | 1/12 (8.3%) | Run later failed balance read |
| expanded02, 0.5/s repeat 1 | 10 → 10 / 9 | 581.396 | 4354.429 | 4354.429 | 5/10 (50%) | 1 post-certification read failure |
| diagnostic01, sequential, BENCH ON | 20 → 20 / 20 | 470.090 | 1596.251 | 4192.191 | 3/20 (15%) | Separate diagnostic, not headline |

At 0.5 offered actions/s, expanded01's two windows observed 0.5 certified
and per-action-checked actions/s, with no sustained queue growth. Expanded02's
first window observed 0.5 certified but **0.45 per-action-checked actions/s**;
one balance read failed (10% of that window, 1/22 over its complete run).
Both runs failed the latency gate before the 1/2/4 steps, which were not
run. Thus **highest measured sustainable rate meeting the target: none
established**, not a claimed zero maximum capacity or a throughput ceiling.

expanded01 includes 18 authenticated one-unit fills, 12 resting bids and
10 cancellations. expanded02 includes 10 fills, eight resting bids and
four certified cancellations, one without the subsequent balance proof.
The first successful expanded01 window delivered two fills/20 s = 0.1
fills/s; actions/s is not fills/s. No timeout or dropped offer occurred in
these healthy runs. pilot03 had one known pre-admission refusal and no
exact-action retry. expanded01 had three known pre-admission refusals;
expanded02 had two exact-action retry attempts, retaining original starts.

## Failures retained, not erased

1. `pilot01`: sandbox rejected loopback binding; no measured workload.
2. `pilot02`: minimum-depth fixture expected exactly height 163 but normal
   staking progressed 162→164. Test-only commit `8cb07f0` accepts honest
   overshoot at that minimum; exact bootstrap authority rules unchanged.
3. `expanded01`: all 40 actions certified and buyer balances checked, but
   the final signed-payload check joined by bare ActionId. V1 ActionId does
   not include market; its signed domain/config does. Independent offline
   inspection found 17 valid cross-market repeated IDs, 52 distinct
   `(market, ActionId)` groups and no changed bytes within any group. All
   40 measured actions matched their own retained payload. Commit
   `0bf1cf7d0075dcc70806670493edf7d730b274b2` corrects only harness/analyzer
   correlation and adds a regression that still catches changed bytes
   **within** a market. Original failed JSON remains unchanged; maker
   conservation was not reached. This is not a fully qualified run.
4. `expanded02`: sample 15's cancellation certified at 1,159.070 ms;
   the subsequent account read failed with RPC -1, `Reported history is
   stale/out of order`. `ReportedHistory` in `src/node/flowmesh_client.cpp`
   rejects a row at/above the authenticated snapshot's next sequence or
   duplicate/non-descending history. It returned no partial account result.
   This is an observed defensive refusal, not evidence of accepted balance
   corruption. Separate server snapshot/status/history reads permit a
   timing mismatch, but missing response bodies mean the exact cause is
   **not proved**. Escalation/repeat/burst stopped; no guard was weakened,
   production repair made, or repeated run used to hide it.

## Replica application tail

pilot03 submission→last exact replica-history observation: p50 **833.415**,
p95 **1005.795**, maximum **1015.051 ms**. Individual replica medians:
node0 446.846, node1 396.072, node2 833.415, node3 431.555 ms. Exact durable
timestamps are unknown with BENCH disabled.

expanded01 sustained-window last-replica p50/p95/max:
526.555/614.962/614.962 and 555.736/1627.487/1627.487 ms. Burst:
2548.819/3135.291/3135.291 ms. expanded02 sustained: 658.546/4708.827/
4708.827 ms, **only nine observations**; failed balance-read action has
unknown replica timing. It is not silently counted as fast.

## Three largest measured diagnostic brackets

Ranked by mean per bracket over repeated calls; **not three independent,
additive causes**. BENCH overhead and host interference are present.

| Bracket | Count | Mean | p95 | Maximum | Meaning |
|---|---:|---:|---:|---:|---|
| Pending tick → worker dequeue | 310 | 45.588 | 229.128 | 1745.419 | Pending-service elapsed time, not pure mutex or CPU time |
| Agreement receive prefix | 783 | 21.881 | 71.903 | 740.272 | Receive/Pump includes execution, journal and relay work |
| Final publication | 80 | 9.130 | 16.617 | 26.073 | Verification/re-execution/serialization/sync write/application |

Other observed spans: execution maximum 230.679 ms, candidate persistence
35.686 ms, certificate assembly 6.769 ms, attestation verification 4.401 ms,
market-mutex wait 2.828 ms. These measurements do not establish disk or BLS
as the principal cause of the long tail. Submit relay upstream p50 1.290,
p95 3.809, max 4.054 ms excludes incoming TLS and is not full network RTT.

The 4,192.191 ms diagnostic outlier (ActionId `51cb5d30…614e3af1`, final
sequence 12) was admitted at 27.125–28.008 ms, had an outgoing view-0
proposal observation at 64.178–65.032 ms, and a 224.486 ms execution
bracket. A following **1,282.932 ms unpartitioned gap** ends in an outgoing
VIEW_CHANGE observation, not a proposal. First durable replica:
4153.255–4154.109 ms; last: 4264.074–4264.922 ms; all-replica read:
4284.760 ms. No retry was recorded for this action. **A two-second timeout
is not established as its cause.**

Diagnostic runtime events identify seven proposer-ingress and thirteen
nonproposer-ingress measured actions by same-epoch seat mapping and actual
candidate creation, not node-number guesses. The final measured request
entered node0 and was proposed by node3. It completed at 1596.251 ms after
one retry of its **identical signed instruction**, initiated at about
1047 ms. No new semantic instruction/trade was added to wake it, but this
does not prove final-message progress without a subsequent retry request.
These are runtime observations, not independent re-verification
of raw proposer signatures; this mapping is specific to diagnostic01.

## Minimum next work, not implemented here

1. Reproduce/capture the authenticated-snapshot versus reported-history
   refusal with bounded response tracing. Keep the rejection rule; isolate
   whether atomic endpoint export or client report-version pairing needs
   correction. This is an availability gate before a clean useful-work
   throughput claim, not a reason to accept unverified data. Also account
   for status-poll traffic in the existing public API request budget; an
   order rate is not the HTTP request rate. Preserve the admission limits.
2. Instrument the worker queue and agreement Receive/Pump boundaries with
   exact operation/market/candidate IDs, including actual store-write entry/
   return, signing, TLS transmission and client validation. The measured
   worker delay is a stronger first target than changing BFT family or
   reducing every timeout. Do not optimize an unexplained gap by guessing.
3. Evaluate bounded isolation of safety-critical agreement work from
   blocking/repeated execution and client work, without moving signing or
   publication ahead of required durability. Persistent HTTPS and reducing
   redundant snapshot reconstruction are separate candidates, not measured
   gains. Re-measure on a controlled idle host and then approved separate
   hosts. No predicted 200 ms result is promised.

No live wallets, keys, validators, bridge calls, production deployment,
push, PR, consensus redesign or activation occurred. No independent security
audit is inferred from this benchmark.

## Separate fault run: stopped at an API refusal

`fault01`, harness commit `5914516ada16cbb31f680a6cb20660f27f1ed889`,
seed `2026092404`, **BENCH ON**, one market/four takers plus maker. Do not
combine its samples with the logging-off headline results.

| Phase | Offered / certified / per-action checks | p50 | p95 / p99 / max | >600 ms | Per-action completion rate |
|---|---:|---:|---:|---:|---:|
| All four, before fault, 24 s | 12 / 12 / 12 | 541.905 | 3962.661 | 3/12 (25%) | 0.5/s |
| Node3 gracefully stopped, 12 s | 6 / 6 / 5 | 821.909 | 4514.281 | 4/6 (66.7%) | 0.25/s inside window; 0.387/s through drain |

The offline phase certified four of its six actions inside the offer
window (0.333/s), all six by drain. Five account results were verified;
only three completed inside the offer window. Three matched one-unit fills
were authenticated; the fourth fill instruction had certified inclusion
but no successful post-fill balance read. Last required-live-replica
observation over five actions: p50 869.136, p95/max 4566.006 ms.
Node3 application remains **unknown/unavailable**, not counted among those
three live observations. B3 advanced 210→246 before the fault and 246→261
on all three remaining nodes during it.

Sample 14 certified at 2768.589 ms, then its account RPC failed with
`Public trading request budget exhausted before admission`. The public
TradingApi guard returns HTTP429 before parsing/dispatch, with fixed
128 global and 32-per-peer one-second request budgets. The retained RPC
error proves rejection by that guard, but not which budget branch was
exceeded. All loopback relay requests share a source address; the existing
five-ms polling loop can generate many reads per submitted action. This
is a real refusal under this harness's HTTP workload, **not proof that
0.5 orders/s alone exceeds operator capacity or that quorum stopped**.
724 action-status requests were observed across setup and both phases;
per-second response traces were not retained. No limit was raised or
polling delay tuned to obtain a pass.

One known pre-admission refusal occurred in the offline phase. Two exact
signed retries occurred before the fault. No timeout/dropped offer was
recorded. The scenario stopped after the failed phase; the report remains
`fault_scenario_pass=false`. **Held/throttled bulk, actual returning-seat
catch-up, accepted post-rearm signing and recovered trading were not
reached.** Dedicated proposer failure, hostile ingress, larger sets and
WAN remain unqualified. No spare live VPS was assumed authorized.

The unexecuted continuation is retained in the harness. It distinguishes
exact caught-up history, explicit rearm/eligibility, and a share accepted
by another node; an armed status is not signing proof. A configured bulk
hold is not assumed to prove that every recovery path was blocked. All
scenario data is retained without resets. No additional run was made to
replace these failures with a green result.

## Reproduction and retained evidence

Commands below are run from the repository root on a compatible build
host. Use fresh temporary directories, run scenarios **sequentially**, and
keep generated secrets/private datadirs out of Git. A default-path daemon
launch is not part of this procedure.

```sh
perf_root=$(mktemp -d /tmp/b3-flowmesh-perf.XXXXXX)
mkdir "$perf_root/source"
git archive f3f2b86840df97c6666379c6c96d4fc26c786213 | tar -x -C "$perf_root/source"
cmake -S "$perf_root/source" -B "$perf_root/build" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_PREFIX_PATH=/opt/homebrew \
  -DBUILD_GUI=OFF -DBUILD_TESTS=ON -DBUILD_BENCH=OFF -DWITH_CCACHE=OFF
cmake --build "$perf_root/build" --target b3coind b3coin-cli b3coin-wallet -j12

# Correctness checks do not become optional under --latency-observe-only.
python3 test/functional/feature_flowmesh_latency.py \
  --configfile="$perf_root/build/test/config.ini" \
  --tmpdir="$perf_root/pilot" --nocleanup --portseed=1421 \
  --randomseed=2026092401 --latency-warmup-pairs=2 \
  --latency-measured-pairs=10 --latency-production-logging --latency-observe-only

python3 test/functional/feature_flowmesh_performance.py \
  --configfile="$perf_root/build/test/config.ini" \
  --tmpdir="$perf_root/expanded" --nocleanup --portseed=1421 \
  --randomseed=2026092402 --performance-markets=2

python3 test/functional/feature_flowmesh_latency.py \
  --configfile="$perf_root/build/test/config.ini" \
  --tmpdir="$perf_root/diagnostic" --nocleanup --portseed=1421 \
  --randomseed=2026092403 --latency-warmup-pairs=2 \
  --latency-measured-pairs=10 --latency-observe-only

python3 test/functional/feature_flowmesh_performance_faults.py \
  --configfile="$perf_root/build/test/config.ini" \
  --tmpdir="$perf_root/fault" --nocleanup --portseed=1421 \
  --randomseed=2026092404 --performance-markets=1 --performance-diagnostic

python3 -B test/functional/flowmesh_performance_metrics_test.py
python3 -B test/functional/flowmesh_performance_fault_metrics_test.py
python3 -B test/functional/flowmesh_performance_analyze.py \
  "$perf_root/diagnostic" --output "$perf_root/diagnostic-analysis.json"
```

The executed interpreter was `/opt/homebrew/bin/python3.14`. Harness
revision for expanded02 is `0bf1cf7d0075dcc70806670493edf7d730b274b2`;
expanded01 used `e400e37`; pilot03/diagnostic01 used `8cb07f0`.
Hardware/scheduling randomness means these commands reproduce the method,
not guaranteed identical timing, generated identities or failure schedules.

Private retained evidence directory basename:
`flowmesh-performance-baseline-20260924.SZoscZ`. Raw reports and hashes:

| File within private directory | SHA256 |
|---|---|
| `pilot03/flowmesh-latency.json` | `491ed400611c5a80a37314ab9daaaf6518b772c2503456d097d4e6fae5e3bf5d` |
| `diagnostic01/flowmesh-latency.json` | `c2c995d492a4a59a6a3034da4f4f15a5e9763a6fc9f6c225a9a526e637274f8f` |
| `expanded01/flowmesh-performance.json` | `f7dfea6ec3dce3387ff30155eeb84378b50203c6c2e0d12a7a0db9796ad6b583` |
| `expanded02/flowmesh-performance.json` | `b83db71892bbf6cb6e37842ff08893462b67bec3517872c14b6ad1fe46864bb2` |
| `fault01/flowmesh-performance-faults.json` | `5c7175586aa40ca08d7054ab0db7408f77669722722ba9170d6c97195b93a126` |

Full configure/build logs, CMake cache, run launch logs, individual node
logs, exact signed instructions, and derived diagnostic JSON are preserved
locally. Do not publish generated wallets, RPC cookies, TLS keys or signer
journals. The report and synthetic test source contain no such material.

Harness exits: pilot03=0, diagnostic01=0, expanded01=1, expanded02=1,
fault01=1. Each launched test child exited **0**, explicitly recorded by
`PREAGREEMENT_TEST_CHILD_EXIT`; fault node3's earlier graceful exit was
also captured as 0. All expanded/fault harness threads stopped. A generic
framework message about dangling processes does not override these exact
child-exit records. Failed runs are not passing qualification merely
because their children shut down cleanly.

Offline bookkeeping qualification: **18 tests passed**, nine general
metrics/clock/market-scoping checks and nine mocked fault-readiness,
exact-target and cleanup checks. The latter execute the actual cleanup
tail with mocked threads/RPC; they do not qualify real catch-up or thread
termination. Commit `cb63d0853131ade075d72a1ee4b95494e355f3e6` adds those
tests without modifying the already executed fault harness. The final
report commit adds documentation only. No V2 model-suite runtime is used
as a performance result.

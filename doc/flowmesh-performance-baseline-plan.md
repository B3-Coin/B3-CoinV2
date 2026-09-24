# Current executable performance baseline — 2026-09-24

This is a test plan, not a performance claim or a protocol change.
Frozen starting revision: `f3f2b86840df97c6666379c6c96d4fc26c786213` on
`flowmeshV2-dev`. The Python V2 models are not the measured executable.

## Identity and isolation

Build the current C++ executable as `RelWithDebInfo` (`-O2 -g`), headless,
wallet enabled, without sanitizers or disabled crypto/durability checks.
Record the final source revision, executable hashes, dependencies, build
configuration, hardware and contemporaneous host load with the results.
Use four independent regtest operators and one ordinary engine-off client,
generated fixture identities, fresh datadirs, independent FMN2 TCP, verified
HTTPS with a generated local CA, and no live node or wallet. This is LOOPBACK
on one Mac, not WAN. The fresh markets opt into the existing experimental V1
pre-agreement mode; unchanged V1 final certificates remain the result.

Preserve the header-repair review target and pending review separately.
Benchmarking does not approve that repair or demonstrate V2 performance.

## Predetermined campaign

1. One harness pilot: existing sequential latency fixture, two warmup pairs,
   ten measured bid/cancel pairs, production debug logging disabled, seed
   `2026092401`. Failure is retained; correctness failures stop that scenario.
2. Expanded workload: four generated taker accounts and a funded maker;
   matched bids, non-crossing bids, cancellations, and two fresh markets when
   the existing fixture safely supports their joint bootstrap. Distinguish
   certified inclusion from verified account changes and reported fills.
3. Sparse/idle and final-request cases; ingress role must be observed, not
   inferred from the endpoint number. No extra request is injected solely to
   wake the final request. Setup deposits are not performance samples.
4. Open-loop sustained load: rates 0.5, 1, 2, 4 actions/second, 20-second
   offering windows, two repetitions, seed `2026092402`. Escalation stops
   after a repeated target miss, growing queues, failures, or a declared
   resource bound. The scheduler does not wait for completion before offering
   the next action. Account-local queues and their delays remain visible.
5. Separate eight-action burst; separate bounded slow/recovery/catch-up and
   proposer-failure cases where existing fixture controls allow them. No
   signing journal reset, replacement keys, arbitrary quorum change, or
   protocol change to make a failing case progress. Unexecuted cases stay
   explicitly unqualified.
6. One diagnostic trace run of the sequential workload, two warmup pairs and
   ten measured pairs, seed `2026092403`. BENCH logs are enabled only here.
   Do not combine its timing distribution with the headline logging-off run.

Each action has a bounded observation deadline. Record offered, started,
admitted, certified and completed counts, queue length, failures, deadlines,
and actual child exits. A timeout is not silently removed from denominators.
Small samples give preliminary empirical percentiles, not tail guarantees.

## Clocks, result meaning and gates

Primary timing starts at the original client submission call and ends when
the client has verified final V1 certificate inclusion for the exact saved
ActionId. This certifies inclusion, not arbitrary endpoint-reported execution
outcomes. Also report scheduled-offer-to-result, original-call-to-response,
first observed request transmission (or its stated observation bound), and
verified account/fill observations. Preserve original time across retries;
retry only the identical saved instruction under existing client rules.

Working gate: p50 <= 200 ms and p95 <= 600 ms. Report nearest-rank p99,
maximum, percentage above 600 ms, rejections and timeouts as well. A load is
not declared sustainable merely because its successful survivors pass.
Report duration/repetitions, queue stability, offered and completed rates.
No highest passing rate may be inferred if none was measured.

Measure every replica's exact applied certificate separately. Last-replica
time is not the quorum-certification time, and submission-to-last-replica is
not additional replication delay. Read-observation times are upper bounds,
not timestamps of disk completion. Use only same-clock intervals or explicit
clock-calibration uncertainty; missing segments are unknown. B3 progresses
using the fixture's ordinary regtest staking/clock driver during workloads.
Qt presentation and B3 withdrawal settlement are separate, unmeasured paths.

Rank only measured stage intervals in diagnostics. Broad persistence spans
include validation/serialization/re-execution and are not pure fsync cost.
The current HTTPS implementation creates fresh verified connections; do not
invent a persistent-session performance result or omit connection costs.
Freeze baseline evidence before suggesting any optimization.

## Reproduction entry points

Existing pilot/diagnostic: `test/functional/feature_flowmesh_latency.py`.
Expanded workload: `test/functional/feature_flowmesh_performance.py`.
Run directly with the optimized build's `test/config.ini`, unique port seed,
fresh `--tmpdir`, `--nocleanup`, and the declared PRNG seed. The retained
results must record exact invocations and source/binary hashes. Do not upload
generated wallet databases, RPC cookies, TLS private keys or private logs.

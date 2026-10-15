# Bounded native queue attribution — diagnostic stage

Baseline source: a463be47649a9afa52052db2e7913b79fef1c2b8.
No consensus, custody, signing, retry, polling or durability policy changes.
This fixture measures native **V1**, including preagreement and final BLS
certification, not the Python V2 agreement/accounting model.

## Predetermined experiment

- Fresh generated regtest datadirs; four operators, one validator-off client.
- Two stable-test-asset/B3 markets, four buyer accounts, existing standing maker
  liquidity created only during setup. No old signed instructions reused.
- Exactly one matched-fill request, one second idle, then eight simultaneous
  matched-fill requests (one per buyer/market). Preserve original ActionIds.
- Existing polling, client serialization, normal timers, quorum, proof checks
  and synchronous durable writes unchanged. B3 advances during both cases.
- Same compiled executable and seed, sequential fresh fixtures:
  control with capture off, then capture on only after setup. BENCH disabled
  in both. Bounded memory-only RPC tracing enabled identically after setup.
  Full public-body relay capture is off: it writes files during requests.
- No sustained-load/200ms qualification from these nine samples.

The hidden regtest-only RPC `flowmeshtiming start|stop|read` controls at most
32,768 events / 16 MiB serialized bytes per process, not measured Python/C++
heap usage. No synchronous diagnostic logging when BENCH is off.
Use one capture per fresh process. Start/stop bracket the measured workload;
spans crossing those boundaries may be absent. Drain measured work before stop.
Capture drops or existing stream-limit markers invalidate complete attribution.
The collector itself serializes/allocates and takes a short mutex: compare its
observed overhead with the matched capture-off run, not an earlier campaign.

Trace lock intervals identify holders by thread/function and nested spans.
The span end follows lock release and is an upper bound, not an exact release
instruction timestamp. Condition-variable predicate-ready is distinguished
from actual blocking. Steady clocks are calibrated using RPC call brackets.

HTTPS dispatch / TLS completion are NOT socket-write or peer-receipt proof.
Server parsed-request and response-write boundaries are distinct observations.
Exact client kernel-write timing, scheduler CPU-vs-descheduling within a span,
and external TLS relay sub-stage timings remain unmeasured unless separately
captured. Do not infer BLS cost from an unexplained residual.

## Command (run sequentially with distinct tmpdirs)

```sh
BITCOIND=/absolute/build/bin/b3coind BITCOINCLI=/absolute/build/bin/b3coin-cli \
python3 -B test/functional/feature_flowmesh_performance.py \
  --configfile=/absolute/build/test/config.ini --tmpdir=/new/disposable/path \
  --nocleanup --randomseed=24092026 --portseed=92401 \
  --performance-profile=native-queue --performance-rpc-trace
# Second fresh fixture: additionally --performance-memory-trace.
```

Save per-action results, bounded RPC observations and memory-timing-nodeN
files. Retain failures. Never add concurrent stage durations; report intervals,
critical-path waits and missing attribution separately. A small scheduling fix
requires an observed cause, preserved before evidence and identical repeat.

The initial diagnostic pair used the inherited public-body disk capture in
both arms. Preserve it separately; it is not the memory-only comparison.
The harness successor includes the ordinary client (which is not a member of
the four-operator node list) in memory capture and child-exit observations.

## Verified execution outcome observation

The optional `--performance-require-outcome` selects the explicit
`native-result-observation-v1` measurement profile. It preserves the original
inclusion timestamp separately and continues bounded read-only status queries
until the native client verifies the including entry's applied result. A
matched-fill sample requires the exact quantity, price, quote paid and fee of
this existing asset/B3 test profile. Resting orders and cancellations must not
claim a fill. Endpoint history and a certified inclusion alone are insufficient.

Once any verified inclusion has been observed, the harness never retries that
instruction, even if a later fresh status becomes unknown. Later certified
evidence must identify the same entry. Missing/invalid results remain failures
in the denominator. The legacy inclusion profile keeps its former acceptance
checks and clock; its numbers cannot be silently relabelled as this profile.

This observes client verification, not durable client outcome qualification.
The current client intentionally discards receipt labels on restore, and all
verified-return persistence paths still need their separate durability review.
The stronger redesign R1/R2 contract also includes the required durable client
boundary; neither a nine-sample diagnostic nor this flag establishes that gate.

For an outcome-observation pair, add `--performance-require-outcome` to both
fresh-fixture commands above and retain both reports under their own profile.
Do not compare its clock with legacy inclusion medians as a speed improvement.

## Opt-in waited-submission comparison

Ordinary signed-action RPCs default to zero wait. Qt already requests up to
2,000 ms on an eligible remote submission (or a later foreground read-only
status wait after relocking). Therefore the default native-queue probe is not
the opted-in Qt submission path.

`--performance-submit-wait-ms=2000` selects the separate
`native-result-submit-wait-v1` diagnostic. Values must be integer 0..2,500;
nonzero requires `--performance-profile=native-queue` and
`--performance-require-outcome` before fixture setup. Zero/omission preserves
the exact original RPC argument tuple. Nonzero appends the existing wait
argument to the first order/cancel request; it does not change signed bytes,
the ActionId, exact-retry policy or result predicates. All submission wait is
inside the original measurement clock, not removed from latency.

Run sequential capture-off/on fresh fixtures with the same explicit wait.
Report the profile and request wait, not a relabelled default-path median.
The fixture has four validators, of which two serve HTTPS; the report records
both counts. This remains one engine-off client process and nine diagnostic
samples, not a Qt UI, durable-client-result, WAN or 200 ms qualification.

Capture cleanup stops every owned child once and retains its complete or
partial trace before aggregating errors. Drops, stream-limit markers, malformed
stop replies and interrupted start metadata invalidate complete attribution.
An early overflowing child must not hide later children or convert failure into
successful qualification.

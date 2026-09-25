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
  in both. Existing public relay/RPC tracing enabled identically in both.
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
  --performance-profile=native-queue --performance-public-trace
# Second fresh fixture: additionally --performance-memory-trace.
```

Save per-action results, public request/response trace and memory-timing-nodeN
files. Retain failures. Never add concurrent stage durations; report intervals,
critical-path waits and missing attribution separately. A small scheduling fix
requires an observed cause, preserved before evidence and identical repeat.

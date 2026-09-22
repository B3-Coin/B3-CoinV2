# Header-retention and aggregation: frozen before-repair diagnostic

Baseline:
[`00d41abc85c3fc15d6c4191bcafd699cc16b7836`](https://github.com/B3-Coin/B3-CoinV2/commit/00d41abc85c3fc15d6c4191bcafd699cc16b7836).
This extends the previously retained header-growth diagnostic with measurements.
It does not modify model source and is not an independent implementation audit.

## Reproduce

Python 3.14; run from the repair checkout's repository root. Prepare a separate
clean frozen checkout, leaving the repair branch untouched:

```sh
git clone --no-checkout https://github.com/B3-Coin/B3-CoinV2.git ../b3-header-frozen
git -C ../b3-header-frozen checkout --detach 00d41abc85c3fc15d6c4191bcafd699cc16b7836
python3.14 -B test/flowmesh_v2_storage/evidence/header-repair/probe.py ../b3-header-frozen
```

The launcher requires exactly the baseline HEAD and a clean checkout. It is
deliberately not an after-repair regression runner. No wallet, key, service,
transaction or live network is required by the probe.

## Exact schedule and measurement adaptation

The retained original sends 300 distinct, application-valid bodies through
receiver1's normal DATA admission path. Each carries a genuine synthetic
Byzantine proposer0's signed PROPOSE reference. Each iteration uses
`RecoveryTests().body(sim, 1000 + i)`, advances the clock to `i * 31`, then
calls `node.pump()`. The helper does **not** call `tick()` or deliver other
simulator traffic.

That proposal pattern is unchanged. The public copy adds:

1. An explicit source path with frozen-revision/clean-checkout guards.
2. Observations after 100, 200 and 300 inputs.
3. One explicitly labelled invocation of the existing `_aggregate()` at each
   observation, with a Python trace hook counting actual entries into its
   header-loop body.

The trace hook neither replaces the method nor mutates any model table.
The probe asserts that each measurement invocation leaves durable state,
headers/bodies, votes, references/requests/pending work, timers, queued network
events, emitted trace and issued signatures unchanged. There is no quorum to
aggregate in this schedule. These three diagnostic calls are **not** presented
as automatic work performed after every hostile input.

## Observed results

| Inputs | Headers / primary lookup entries | Bodies | Serialized header cache bytes | Actual headers inspected by one aggregate call |
|---:|---:|---:|---:|---:|
| 100 | 100 | 100 | 67,401 | 100 |
| 200 | 200 | 200 | 134,801 | 200 |
| 300 | 300 | 256 | 202,201 | 300 |

There is no separate aggregation index in this baseline. At every observation:
2 references, 0 requests, 0 pending entries, 0 received-vote buckets, 1 accepted
proposal, view0, deadline30, and no signing halt. Tick calls remain zero; the
clock values are 3069, 6169 and 9269.

The serialized-cache measurement encodes an explicit list of
`{key: [view, ValueId], proposal: signed_header}` using the model's canonical
JSON codec. The signed-header-only byte sums are 58,300 / 116,600 / 174,900.
These are reproducible serialized sizes, **not** Python resident-memory sizes.

The measurement launcher exited `0`. It uses the original in-memory agreement
simulator, not child processes or SQLite; no process-kill or disk-recovery
result is claimed here. The original probe hash, exact source/tree, public
file hashes, observations and measurement definitions are in `BEFORE.json`.
Actual bounded stdout is preserved as `baseline-results.jsonl`.

## Scope of the counterexample

The first accepted proposal schedules a timer at tick30. Timer servicing is
deliberately withheld while the synthetic clock advances. This is **not**
the previous R2 150-cycle schedule, which services timers each cycle, and it
does not establish failure under a fair timer-servicing assumption.

It demonstrates two specific resource facts: headers continue growing after
body storage hits its 256-object cap, and a later invocation of the existing
aggregation routine inspects the entire retained header map. The original
source and previous evidence are preserved outside the repair checkout.

The reviewed baseline's synthetic signatures/membership/anchors remain
synthetic. No liveness, global-heap, elapsed-time, WAN or production-deployment
qualification is inferred from these measurements.

## Development AFTER: same original input pattern

The separate `after_probe.py` runs the original DATA-plus-PROPOSE pattern
against an explicitly supplied repaired checkout:

```sh
python3.14 -B test/flowmesh_v2_storage/evidence/header-repair/after_probe.py .
```

It records HEAD, working-tree dirty status and hashes of the model Python/profile
files before execution, then checks that those files and HEAD did not change.
The retained `after-development-results.jsonl` and `AFTER.json` are **working-tree
development observations**, not a claim that their parent HEAD contains the
repair. A final committed-tree run must be identified separately at handoff.

This is not the prepared-first regression fixture. It uses no initial prepared
certificate: prepared count remains zero; one ordinary initial proposal is
accepted. Tick calls remain zero and deadline30 remains overdue as the clock
reaches 3069, 6169 and 9269. No timer, view, quorum or message pattern is changed
to obtain the measured result.

| Inputs | Headers / lookup entries / size-index entries | Bodies | Baseline-comparable serialized list bytes | New budget-accounted entry bytes | Actual keyed lookup observations |
|---:|---:|---:|---:|---:|---:|
| 100 | 2 / 2 / 2 | 100 | 1,349 | 1,344 | 1 |
| 200 | 2 / 2 / 2 | 200 | 1,349 | 1,344 | 1 |
| 300 | 2 / 2 / 2 | 256 | 1,349 | 1,344 | 1 |

The comparable list uses the exact BEFORE encoding above. The repaired cache's
budget instead sums individually encoded `{slot:[view,ValueId],signed:header}`
entries. These encodings differ: do not compare 1,344 directly with BEFORE's
202,201-byte list and call them the same measure. Signed-header payload-only
bytes are 1,166 at every AFTER checkpoint. None is a Python heap-size measure.

A trace hook observes one actual `_aggregate` call, one `_lookup_header` call
and one header-cache lookup statement for the latest candidate per checkpoint.
The model reports one key, two phase checks, zero votes scanned and no proof
construction. The explicit diagnostic call does not change semantic state or
traffic; only aggregation telemetry may update. It is not presented as a
measurement of every automatic operation or a throughput benchmark.

The peak reported header-cleanup scan is 3 entries in this exact pattern,
with at most one header removed per admission. Full counter fields are retained.
These observations are not the worst-case bound or proof of all incoming
schedules. No secondary aggregation queue/index exists in this implementation.
The original BEFORE launchers, results and source identities are unchanged.

## Explicit failing-before / passing-after resource assertion

The separate source-independent checker asserts the synthetic expectation
`headers <= 32` against every retained 100/200/300 observation:

```sh
python3.14 -B test/flowmesh_v2_storage/evidence/header-repair/check_retained_bound.py test/flowmesh_v2_storage/evidence/header-repair/baseline-results.jsonl --max-headers 32
python3.14 -B test/flowmesh_v2_storage/evidence/header-repair/check_retained_bound.py test/flowmesh_v2_storage/evidence/header-repair/after-development-results.jsonl --max-headers 32
```

The first command was executed and returned **exit 1**, reporting FAIL for
100, 200 and 300 retained headers. The second returned **exit 0**, reporting
PASS for 2 headers at each checkpoint. Exact output is retained in
`bound-check-before.jsonl` and `bound-check-after.jsonl`.
The checker refuses missing, duplicate or out-of-order checkpoint observations.

These are explicit checks of the recorded resource observations, not claims
that the repaired test API ran unchanged against the old implementation or
that the independent BFT checker proves resource bounds. The 32-header policy
is a declared synthetic TEST-profile parameter, not a production default.

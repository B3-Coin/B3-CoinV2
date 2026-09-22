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

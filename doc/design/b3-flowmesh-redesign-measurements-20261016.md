# Native redesign diagnostics: failures remain in the result

Date: 2026-10-16. These are deliberately small, predetermined diagnostic
campaigns, not a release, 200 ms, Qt, WAN or durable-client-outcome qualification.

## Fixture and clocks

All rows below used the same Mac/session: Apple M4 Max, Mac16,5, 16 physical/logical
cores, 128 GiB RAM, macOS 26.5.2 (25F84), AppleClang 17, Release C++20. Each fresh
generated regtest fixture has four native validators, two HTTPS-serving operators,
one engine-off client process, four buyer wallets, two markets and standing maker
liquidity. Seed `24092026`, port seed `92401`. B3 advances during measurement.
No existing data, live keys, mainnet or VPS participates.

Each campaign has one isolated matched fill, one second idle and eight simultaneous
fills. The stronger profile measures original client RPC start to client-verified
applied execution outcome; it separately retains first certificate inclusion and
subsequent authenticated account observation. Inclusion, outcome and account
clocks are not interchangeable. Normal cryptography, quorum and synchronous
storage remain enabled, including macOS full flush behavior.

The harness polls local status every 5 ms; this is not Qt's passive polling/UI
workload. Polling coalesces at the client, so local RPC count is not HTTPS count.
The waited profile requests the existing 2,000 ms submit wait and includes its
entire duration in the same original clock. Neither profile qualifies durable
client outcome restoration.

## Actual results, kept separate by campaign

| Native build / profile | Capture | Isolated result (ms) | Burst completed | Burst result p50 (ms) | Outcome |
| --- | --- | ---: | ---: | ---: | --- |
| Baseline / ordinary submit | Off | 249.645 | 8/8 | 2,059.147 | Correctness pass; performance target missed |
| Baseline / ordinary submit | On | 176.708 | 7/8 | Not qualified | One 60-second failure; partial operator capture |
| Immutable reads / ordinary submit | Off | 230.185 | 8/8 | 1,112.664 | Correctness pass; performance target missed |
| Immutable reads / ordinary submit | On | 273.300 | 5/8 | Not qualified | Three failures; all five trace buffers dropped events |
| Immutable reads / waited submit | Off | 240.376 | 7/8 | Not qualified | One 60-second failure; bounded RPC capture dropped records |
| Immutable reads / waited submit | On | 206.687 | 3/8 | Not qualified | Five failures, including missing outcomes; all trace buffers dropped events |

All five owned daemon processes exited zero in every row. Harness failure is not
a daemon crash, and a successful isolated fill is not a population median. Do not
combine successful-only medians, timings from different rows or overlapping stage
durations. The small off-capture comparison suggests a possible burst improvement,
but the captured failures prevent a reliability claim. More measurement is not
a substitute for repairing the demonstrated lost-work path.

Baseline daemon SHA-256:
`76024e48e44cdd60d99545b888a4ad7def04c7b18a43625eb75b98256549bf1a`.
Composition: `46e5cf927b66dadc37ddfac30dce20a7847baf1b` native source plus the
seven chain-lock diagnostic files from `fa49bba`; ordinary service/client.
Immutable-read daemon SHA-256:
`77da7f9bdec6a28c50b63ed6fb735c28d5a53d8102d73c9cfbb6d9a1ed252d87`.
It adds the six native read-isolation files frozen by `1d268c4`; no unqualified
public-service, advisory, client-durability or mutation-fence candidate is present.
Ordinary-submit harness: `6283ffb`; waited-submit harness: `5a58529`.

## What the partial baseline capture establishes

In the baseline captured row only, the largest client work-lock wait is
118.560 ms behind another request's two synchronous saves (44.942 ms combined)
and 73.589 ms HTTPS call. The largest observed market-lock wait is 61.632 ms,
behind message processing whose anchor lookup overlaps a raw 55.288 ms B3 chain
activation-lock bracket. These are observed ownership/wait relationships, not
CPU-time attribution or an additive latency decomposition.

That row's final execution costs roughly 0.4-0.5 ms, final certificate verification
roughly 0.6 ms and final append sync 4.6-10.5 ms. Those operations alone do not
explain second-scale bursts; no BLS/PBFT bottleneck is established from a residual.
Operator capture ends after about 10.5-11.9 seconds, so it cannot attribute the
later 60-second timeout. The native client timing buffer in that baseline row
is complete; its separate bounded RPC capture drops 310 records. These are
different evidence layers, not a claim of complete overall attribution.

An accepted local request can be discarded before pool admission when its old
queued header needs refresh during temporary chain reconciliation. The client
correctly retains global uncertainty, but its queue observation can then remain
misleadingly old. The next bounded repair must preserve the same accepted original
through that temporary condition, with admission reservations, exact ownership,
fixed expiry and existing paced maintenance—not invent a new request, clear an
obligation, weaken authority checks or claim proven non-execution.

## Reproduce

Use the ordinary build and the commands in
[the versioned native queue protocol](../../test/functional/flowmesh_native_queue_protocol.md).
Run distinct fresh directories sequentially, never loop until green. Use
`--performance-require-outcome` for all these rows; add
`--performance-submit-wait-ms=2000` only for the separately labelled waited pair.
Do not run heavy builds/tests during the measured windows. Preserve raw reports,
all partial traces and actual child exits privately; do not publish generated
wallets, TLS keys, credentials or private logs.

## Subsequent bounded-retention repair

The lost-work scheduling repair was subsequently qualified and committed at
`f1744e5`. Its [fresh diagnostic pair](b3-flowmesh-retention-native-diagnostics-20261016.md)
completed 9/9 applied-result observations in each arm, with no capture drops.
Those separate results do not replace the six historical rows or qualify the
200 ms target; the new report retains their still-slow burst timings and exact
remaining attribution gaps.

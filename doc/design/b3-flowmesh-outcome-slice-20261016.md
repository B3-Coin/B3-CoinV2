# Bounded client result-decision diagnostic

Date: 2026-10-16. Measurement profile `native-outcome-decision-slice-v1`,
test implementation revision 2. Bounded source re-review PASS; 27 offline
controls passed. One actual daemon experiment subsequently completed below.
This is not a performance, durability, reliability or release qualification.
The [failed earlier capture](b3-flowmesh-post-admission-diagnostics-20261016.md)
remains unchanged and unexplained.

## One predetermined experiment

The additive [entry script](../../test/functional/feature_flowmesh_outcome_slice.py)
inherits the frozen `f344f813` performance workload. No signing, arrival,
retry, status-poll, action deadline or setup method is copied or changed.
Use one isolated fill, one idle second and eight burst originals, the same
four generated validators/two TLS endpoints/engine-off client, seed 24092026,
zero SubmitWait and the original 60-second deadline and five-millisecond polls.
The selected original is sample 3, bound to its actual fresh MarketId/ActionId
pair and account from the original response. Success is accepted as success;
there is no reselection or rerun until failure.

Only client node 4 records native memory timing, starting before the burst.
All validators remain uncaptured; generic RPC/public-body capture is disabled.
At most two compact target observations survive: first verified inclusion and,
unless it already has a verified outcome, its next existing status return.
No extra action, status, retry, endpoint-availability or polling call is added.
Main-thread control RPCs and post-stop file writes are separate diagnostics.

Stop becomes eligible after the selected return, or after ten seconds. It is
checked on existing main pump turns; blocking pump/control calls can delay
the actual stop. Eligibility, requested stop and overshoot are recorded, not
claimed as a hard ten-second bound. Owned stop is one-shot even after failure;
a preexisting generation is refused, never cleared. Missing/ambiguous start,
stop, native fields, clock correlation or cleanup makes the diagnostic incomplete.

Native caps remain 32,768 events and 16 MiB of serialized data, not total heap.
Require zero dropped/field-error observations and no limit marker. Correlate
only a complete exact-pair ActionStatus wholly inside its capture generation
and conservatively inside the chosen RPC bracket. Other in-flight/later
emissions are censored, not inferred absent from a zero-drop sink.
BENCH/debug categories stay off through relevant completion.

The unchanged inherited Preagreement shutdown checks actual return codes and
logs final `PREAGREEMENT_TEST_CHILD_EXIT` index/PID/status for all five daemons.
Require those final observations; `native-child-exits.jsonl` contains setup
restarts plus the final client, not the final four validators. No cleanup
override or new background worker is introduced.

## Retained review counterexamples

The first private version passed 24 offline controls but was rejected before
any daemon run. Review found a legitimate first status could obtain inclusion
and outcome without entering UpgradeOutcome; its absent decision marker was
incorrectly rejected. Missing parent span IDs could also permit a false
no-HTTPS-child conclusion. Code 13 could retain a stale receipt reason, which
must not establish a standalone BUDGET cause.

Root overlaid the three new controls onto that frozen old helper/entry:
27 tests ran, actual exit 1, nine failed subtests/assertions and one error
across the new cases. The 24 old control bodies were unchanged. The corrected
helper accepts first verified success without inventing an upgrade marker,
requires valid numeric span identity before graph attribution, and labels
code 13's exact cause unproven from receipt reason alone. Second unverified
status still needs its marker. Only code 14 proves recognized standalone BUDGET;
code 10 is pretransport reservation and code 11 is no HTTP response, not delivery.

Author, separate reviewer and root each ran the corrected 27 stdlib controls
once with exit 0. These use an inert fake parent and cannot prove real native
emission, transport, clocks, zero real drops or child exits. Source SHA256:
entry `c2e3e99baf291760c6da514e45cd38e1ffe7420316c68d873f68a5195f4496e1`,
helper `f5615effdf018f2519a48eb8cde9c71765de77afa6bbc7a600a7455bc6a0390f`,
controls `b44ba28bbb3f121325909e55070775ca8748394b9eb0aa5f1e3085ee2d04f413`.

From the repository root, reproduce only offline controls with:

```sh
python3 -B test/functional/test_framework/test_flowmesh_outcome_slice.py -v
```

The real fixture additionally requires a locally qualified ordinary daemon,
its matching build configuration and fresh generated directories. Never use
an existing wallet/datadir or substitute a live endpoint. The fixed selected
experiment's daemon is the preserved Catalog candidate SHA256
`06f5ddfdf80ffd8e472efd69aacee52978a605a69082575d7f05dedfaf71a95c`,
client source `bff8dac1`, with port seed 92501. No real result, 200 ms success,
old-failure cause, WAN behavior or deployment is inferred from a planned run.

## Actual fixed-input observation

The first launch used an older scratch driver `97c0efbc` and exited 2 at
argument parsing: it did not recognize the required outcome option. No test
directory, node, wallet or original request was created. Its stdout/stderr,
exit manifest and stale driver were retained. The corrected launch used the
verified repository driver `f344f813` and an explicit configuration naming
that source, the same native binary, seed and port seed. It is the one actual
fixture experiment, not a retry of an unknown signed instruction.

Actual parent exit was 0, with no timeout or bounded-output overflow. All nine
originals obtained verified applied/fill results and authenticated follow-up
balances; maker conservation and read consistency passed. All harness threads
stopped, and final inherited stdout records show actual status 0 for each
index 0 through 4. The old failed campaign is not reclassified by this result.

The isolated fill took **307.772 ms** from original submission to verified
result. All eight burst results completed: nearest-rank empirical p50
**1,364.825 ms**, conventional eight-sample median **1,462.619 ms**, maximum
**2,274.240 ms**. These small, heterogeneous observations are not a sustained
load, tail-latency, capture-overhead or 200 ms qualification. The inherited
performance flag was false. No controlled speed comparison with another
campaign is made.

The selected client slice is complete for its chosen call: 178 events,
64,226 serialized bytes, **zero drops**, 1,480.537 ms of capture, debug/BENCH
off, no observer errors and exactly one conservatively matched completed
ActionStatus. It found decision **9**: the unchanged internal client automatic
read budget declined that result-evidence attempt. The endpoint advertised
support and the client retained 764 bytes of inclusion proof. That selected
status had zero HTTPS descendants and no measured work-lock wait at
microsecond resolution. This was not a standalone server BUDGET response
(decision 14), a missing proof, or evidence that validators were waiting.

For this target, first verified inclusion was at 1,367.843 ms and the verified
execution result at 1,560.413 ms, a 192.570 ms interval. The slice covers the
next existing status call, not that entire later interval. It establishes
one concrete budget deferral, not that all 192.570 ms—or the whole burst—was
caused by the budget. Later emissions outside the stopped slice are censored.
The eventual result is retained separately; no further target or campaign was
chosen to force a failure. Why the old five results timed out remains unproven.

Retained artifact SHA256 values are:

- Full workload report: `ae26cbe776dce8997ed2d8b2ebb199f53fef2b7f85d66362edd965f338f3d67a`.
- Compact decision report: `8a679bb9bf6228837d3295e5663e798f37060669d534256441c395607c4fe33f`.
- Bounded native capture: `b9385dc5f073ea00086e352e3d38ef05650cf27d11ddaac83c2acf8cd5aa9f73`.

Generated wallets, TLS keys, journals, databases and raw output directories
remain private. No live wallet, VPS, mainnet, bridge, release or activation
operation occurred.

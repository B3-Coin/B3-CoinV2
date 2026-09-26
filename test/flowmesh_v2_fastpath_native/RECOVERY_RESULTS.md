# Native recovery continuation — 2026-09-26

## Result and exact scope

The next two gates are implemented: the delayed-prepared-evidence model repair,
and a native BLS/execution/disk-backed recovery harness. This is NOT a full-node,
Qt, WAN, or externally deployable V2 result. No live node, wallet, key or journal
was used. Production consensus and settlement remain untouched.

Branch: `flowmeshV2-dev`.

* Model correction: `cc2b786d4dcbc1c5344ebb613d76f37378a48cc0`.
* Frozen native source tested: `f17fdabb8cc6c5e8e5249e527aac600466b57d64`.
* Strengthened scheduler: `bb00b96e6ae40db70c57d1bcdc0d5b0e6adff855`
  (native C++ sources unchanged).
* Native worker SHA256:
  `df89eef190f2a4bb07febb8f37eec4241c28440d0a97ddad15acb839c09a0415`.
* Build: AppleClang 17, RelWithDebInfo, Apple M4 Max / macOS26.5.2; default-off
  UNIX test targets. No timing distribution is measured by this campaign.

See [versioned profile and commands](RECOVERY_PROFILE.md),
[sanitized case results and raw-capture hashes](captures/recovery-20260926.json),
and [the genuine full-node integration boundary](INTEGRATION_GATE.md).
Later report-only commits do not change the tested source.

## Causes found and repaired

| Finding | Reproducer / before result | Correction and after evidence |
|---|---|---|
| A lagging view0 node retained PC1 then issued invalid REPORT1 carrying it. | Original model: four failures, one error, one passing negative control. Honest delayed delivery sufficed; no corrupt store. | Verify retained PC and target at least PC.view+1; finite-bound exhaustion preserves obligations. Six regressions, all64 research tests; native future-PC case reaches REPORT2 and later receives the original decision. |
| State serialization/size failure occurred outside the persistence-fencing catch. | Inject failure after signature computation: original path returned with signing not fenced. | All serialization/write failures fence. The whole compute/staging/persistence transition also fences exceptions before the database call. Synthetic insertion failure covers this boundary, not actual OOM. |
| Fencing alone still leaked a computed-undurable PREPARE through status.v0. | Strengthened regression failed: `failed-store status exposed proof-bearing mutable state`. This was a real signature in diagnostic output, not an emitted network event. | Failed-store responses suppress mutable fields and buffered events. Scan all pre-restart output for the recovered signature, not merely events. Exact durable intent resumes after reopen; no replacement instruction. |
| Restart accepted missing historical REPORT authority or a mutilated COMMIT guard. | Old worker accepted the damaged record set; independent observer rejected lost records or changed immutable guard. Both sequential REPORT1→REPORT2 and local NEW_VIEW whose quorum excludes its own REPORT were reproduced. | Require historical prior-view authority, own NEW_VIEW's same-view REPORT, and COMMIT's retained highest-PC predicate. All four targeted missing-evidence cases safely refuse. |
| Observer accepted a bare application marker and did not independently cross-check restored obligations. | Negative controls covered certificate-free application, forgotten original/highest evidence, wrong identity and count regression. | Thirty observer controls now pass. Preserved 28 initial schedules were replayed unchanged, then the final native campaign ran against the strengthened checker. |
| Preliminary hidden-FAST schedule released old PREPAREs, allowing old FAST to reform. | The earlier successful cases were recovery by redelivery, not evidence of SLOW selection. | Preserve that capture as preliminary. Keep standalone old votes/certificate withheld during REPORT/NEW_VIEW; require actual SLOW on each honest replica before delivering late FAST. No protocol change. |

These are defects caught in the new isolated model/prototype. They are NOT a
claim that these repairs recover previously locked live V1 markets.

## Newly executed native evidence

* 46 cryptographic/codec verification assertions passed.
* 32 separate-process recovery scenarios passed: healthy FAST, failed original
  coordinator, hidden FAST plus equivocation, missing-body recovery, eight
  interruption points, two injected failure paths, late PC while behind, two
  ordinary view timeouts and 16 predetermined shuffled schedules (seeds0–3).
* Every successful recovery reopens the same exact test stores, without another
  client request, and verifies the same result with application count exactly1.
* Six store/ownership checks passed: missing prior REPORT, local NEW_VIEW without
  its REPORT, missing original V0, missing COMMIT highest-PC guard, altered
  application snapshot, and simultaneous ownership refusal.
* Captured worker exits: 281 normal exits0, eight intentional cut exits73, seven
  intentional SIGKILL exits−9. The duplicate owner was separately refused with
  exit1. No unexpected failure-cleanup termination appears in successful cases.

The hidden FAST test keeps all issued signatures in the observer even when the
certificate is withheld. Recovered SLOW selects the same exact value, and a
late original certificate cannot duplicate execution. Native view2 coverage is
explicit: the delayed-PC case and second-timeout agreement, not inference from
view1 eligibility.

A separate reviewer replayed the strengthened hidden-certificate capture:
hidden FAST existed before timeout; NEW_VIEW1 carried reports x,x,y plus
equivocation evidence; all three honest replicas reached SLOW1 before the
explicit late FAST deliveries. Final application counts remained1 after reopen.

Full standard-library qualification has 446 tests: 113 accounting, 142 earlier
agreement/recovery, 69 earlier storage, 8 runner controls, 20 coordinator,
64 fast-path research, 30 native-observer controls. The native process campaign
is separately built/run; Python CI does not compile or qualify these C++ tests.

## Review and preserved failures

Separate read-only contexts reviewed the proof codec and native worker. The
worker review found the diagnostic signature leak and two restart-evidence
gaps; the final boundary rereview at `f17fdabb` found no remaining blocker in
those reviewed paths. This is bounded implementation review, not a formal BFT
proof, external security audit or production approval.

Original model failures, initial compile diagnostics, 28-case preliminary
campaign, before-fix serialization/status failures and old-worker corrupted-
store failures are retained privately with their original databases and output.
Public capture contains sanitized results/hashes, not runtime databases or keys.
The old native executable remains preserved for before/after reproductions.

## Performance and next gate — do not conflate

The earlier healthy native-core measurement remains approximately 79 ms median
on this Mac; one run retained a 1.625 s outlier. It used a different proof format
and had no native view-change/recovery. It is NOT a measurement of this new
worker, stock daemon or Qt. See [the original unmodified captures](RESULTS.md).

This worker uses a fixed membership, two credential-distinct but economically
identical bodies, one sequence and finite views. Synthetic custody/anchors,
controlled pipe delivery and observed guards are declared assumptions. Actual
power loss/OOM, coherent backup rollback, arbitrary-load availability, economic
conflict batches, multi-instance agreement, full-node bootstrap/client proof,
Qt, WAN, PoS V2, bridge compatibility, futures and hosting fees are unqualified.

The next implementation boundary is an explicit regtest-only bootstrap/store/
client-verifier adapter. Existing V1 certificate and settlement validation must
not be bypassed or fed re-labelled TEST signatures. Only a real engine-off
client result with B3 advancing can establish the next wallet-level latency.

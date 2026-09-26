# Local native FAST-core measurement — 2026-09-26

**Result: approximately 79 ms median in the isolated native core, NOT in the
current wallet/full node. Native failure recovery and group testing remain open.**

Tested source: `2aa8587b9b9e449fbe9928e3e73c3edb4faed97f` on `flowmeshV2-dev`.
Executable SHA256:
`f814d8019ead50b77981cfe7de59cb39157130025d3be0c59bc5676072410238`.
Apple M4 Max, 16 CPUs, 128 GiB, macOS 26.5.2 (25F84), AppleClang 17.0.0,
RelWithDebInfo (`-O2 -g`). No node, model suite or build was launched concurrently
with the two timing campaigns. This was the user's ordinary Mac, not a controlled
dedicated bare-metal laboratory; unrelated OS/background activity was not traced.

See [versioned profile and exact reproduction commands](PROFILE.md) and
[sanitized per-request captures, stages, hashes and exits](captures/local-20260926.json).
All generated databases and earlier smoke/failure records remain in private local
evidence; no wallet, private runtime directory or generated secret is published.

## What actually ran

Four independent native replica processes and a fifth engine-off native client.
Production curve-auction execution and original Schnorr credentials; real BLS
verification/aggregation; loopback TCP; synchronous LevelDB intent, exact vote,
decision plus full resulting snapshot, and client-outbox writes. Every request
was checked for a real one-unit matched fill and exact fee/account deltas.

The test-specific consensus is fixed-leader view-zero FAST only: three distinct
votes including the leader. It is not complete P2-FV, PBFT, the existing runtime,
or a replacement for the Python recovery model. Its certificate signs a separate
TEST domain. There is no native view-change/leader-replacement path. Missing
quorum/disconnection causes refusal, never timeout unlocking.

Both campaigns used the same frozen binary: two retained warmups followed by
40 measured serial requests, established connections and preloaded maker
liquidity. All-replica completion was awaited between offers. This is no-load
latency, not throughput, burst, WAN or production availability qualification.

## Results (each campaign separately)

Milliseconds, nearest-rank p95; no samples dropped or winsorized.

| Boundary | A median | A p95 | A max | B median | B p95 | B max |
|---|---:|---:|---:|---:|---:|---:|
| Original signing → verified final receipt | 61.668 | 80.452 | 1377.839 | 61.716 | 71.402 | 85.806 |
| Above **including client terminal sync** | **78.6065** | **97.896** | **1625.114** | **78.833** | **89.928** | **113.536** |
| Original signing → observed all-replica application | 80.3355 | 99.256 | 1627.499 | 80.930 | 93.670 | 122.257 |

The primary number includes original instruction signing and pre-send outbox
sync, actual matching/agreement, durable leader decision and balances, returned
certificate verification, and the client's final durable receipt. It is not
merely admission, signature generation, socket write or an auction microbenchmark.
The last row includes all preceding work and observation delay; it must not be
reported as additional replication delay or added to another row.

The earlier ~445 ms full-daemon result includes different runtime, client,
transport and B3 work. These numbers are not an apples-to-apples speedup ratio.
The earlier ~161 ms research simulation is also a different measurement.

### Do not hide the two slow samples

Campaign A, index 20 / entry 21: client-durable 1625.114 ms. Original outbox was
ready at 5.150 ms, result received at 726.453 ms, proof verified at 729.165 ms,
client terminal write returned at 1625.114 ms. That last write call occupied
895.949 ms. Leader decision+snapshot sync occupied 431.714 ms; its quorum wait
was 273.463 ms. Follower sync calls also occupied hundreds of milliseconds.

Campaign A, index 21 / entry 22: client-durable 1398.251 ms. Original outbox sync
occupied 222.861 ms; leader intent sync 222.639 ms, signed-record sync 220.543 ms,
quorum wait 692.493 ms. Follower durability calls account for much of that wait.

These are **observed synchronous-call durations**, not a proven OS/device cause.
No fsync syscall scheduler/device trace was captured. Concurrent replica durations
overlap and are not added. The build detects `HAVE_FULLFSYNC=1`; LevelDB attempts
macOS F_FULLFSYNC with its documented fallback. Actual machine-power-loss safety
and the physical storage/controller path were not tested. Durability was not
relaxed to improve timing.

For context, per-request leader execution median was ~1.22 ms in each campaign;
leader quorum-wait median was 30.825/30.338 ms. These stage medians do not compose
into a valid end-to-end median. The raw per-request stage arrays are retained.

## Correctness and lifecycle checks actually executed

- 84 real fills (80 measured, four warmups), all with valid original ActionIds,
  receipts and required balances. An earlier three-fill smoke run is separate.
- All ten timed server/client processes exited 0 and emitted clean shutdown.
  Four stores per campaign reopened in separate processes; each had exactly 42
  decisions and the same state root. Both independent campaigns ended at
  `a06e3b9fadfb3a7e08d3145520294108da6f5230aaeea7460d3d68a1021ce290`.
- Client stores reopened with the same 42 original signed requests, sequences and
  final certified records; no signing or resubmission occurs in that check.
- Both concurrent attempts to open an owned signer store were refused for the
  actual LevelDB lock reason, not merely any nonzero exit.
- Five deliberate `_exit(73)` cuts: after durable intent, durable signed vote,
  publication witness, atomic decision/snapshot/head, and memory application.
  Reopening and completion exited 0. Existing signed votes remained byte-identical;
  reapplying the certificate did not change the state or decision count.
- Four generated-store faults: missing intent, missing snapshot, inconsistent
  snapshot and malformed signed record. Each reopening returned the expected
  specific safe-refusal reason. No lock deletion or discarded obligation.
- Six negative controls in the direct store exercise: subquorum bitmap, unknown
  seat, altered receipt, altered slot, corrupted signature and trailing frame.
  These exercises are not network Byzantine/failover qualification.
- Existing runner checks: 8/8; coordinator model: 20/20 in 71.807 seconds;
  fast-path research model: 58/58.
  No claim of rerunning the whole previous V1/Qt/settlement campaign.

## Review and small test-harness corrections

Separate bounded read-only reviewer context inspected the actual source. It
identified an API mismatch (WriteBatch throws/returns void), missing applied-state
snapshot in the initial journal design, and leaked iterator ownership. Before the
frozen measured build these were corrected: throwing write contract, atomic full
snapshot plus decision/head, and RAII iterators. Final bounded review found no
new blocker for this declared healthy-core probe. This is not an independent
security audit or approval of a complete native consensus implementation.

Previous GitHub model run 36168474703 failed: its final coordinator test exceeded
the 180-second suite budget. All other suites passed. Local full coordinator
rerun passed; separate commit `31f76f3` raises that bounded suite budget to 300s,
without changing tests, discovery, rejection assertions or the 15-minute job cap.
A later remote CI run must be reported from its actual result. The Python model
CI does not compile or run this native probe.

## Remaining gates

Complete native safe view-change/restart recovery; adversarial certificates and
failure delivery; full node/FM​N2/HTTPS/Qt integration; B3 advancing/reconciliation;
load, bursts and slow replicas; separate-machine/WAN tests. Synthetic custody,
anchors and fixed known membership remain test assumptions. No production
deployment, shared regtest-group package, live keys or host commission support.

**Next bounded step:** implement and adversarially qualify the missing native
recovery path before integrating this prototype into the tester group's regtest
nodes. Keep the 79 ms result scoped to the measured healthy core; measure the
complete client path again after integration.

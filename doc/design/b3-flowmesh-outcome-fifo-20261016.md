# Fair scheduling of included-action result reads

Date: 2026-10-16. Local source-reviewed C++20 stage, basis `facc1e06`.
The three targeted after-controls and the complete selected native/Qt runs pass
within their declared local scope. No trading-latency or release claim follows.

## Cause and minimal correction

An unresolved action already used the bounded exact `(MarketId, ActionId)`
`Demand`/`Take` FIFO. An included action instead removed its queue key and called
`UpgradeOutcome`, taking a raw token. A hot included caller could therefore
take the next replenishment before an older unresolved demand.

In [ActionStatus](../../src/node/flowmesh_client.cpp), eligible outcome-only
reads now join that **same FIFO**. `OutcomeUpgradeEndpoint` extracts the existing
eligibility checks. One call services at most one selected owner. Terminal,
completed, unoffered, missing-proof/endpoint and cooling own outcomes remove
stale demand without spending another owner's token. Missing retired owners
are removed without scanning ahead or sending traffic.

When the selected owner is another included action, its own current B3
authority is rechecked before evidence is fetched. Binding restart obligations
use the existing verified-return guard. After other work, the caller's previously
verified inclusion is rechecked before return. Only that caller's card is
returned; the selected owner's proof, original, outcome or account is not
reassigned. Explicit retry of an already-included action still cannot reach
`Send`; unresolved explicit-retry policy is unchanged.

The work owner, endpoint policy, one-shot terminal rules and recognized
standalone server-BUDGET cooldown remain. Outcome HTTPS still holds the client
work owner; this is not transport isolation or background autonomous progress.

## Unchanged bounds and durability

The scheduler remains backend-global across wallets/accounts, markets and
endpoints: two burst tokens, 62.5 ms ordinary replenishment, admission only
while the one-second window has fewer than 16 charges, at most 512 keys/demands.
Each automatic endpoint attempt is charged before transport; evidence adds
the unchanged post-response costs. Those extras can temporarily exceed 16
recorded charges; this is not a newly claimed hard 16-total-charge cap.
`Take` itself does not charge and `Remove` never refunds. No budget, deadline,
proof-retention limit, quorum, fee, consensus, wire or signing rule changes.

Pure outcome text is intentionally omitted from the restart digest and is not
restored. The persistent guarantees here are exact originals, high-water and
no-resubmit obligations, not a new synchronous result-preimage journal. Reopen
withdraws verification labels until fresh evidence, without resending an order.
The existing verified-return durability guards remain unchanged.

The FIFO is volatile. Abandoned keys remain bounded until ordinary service,
existing removal or restart. Existing duplicate-inclusion, nonexecution and
pending-eviction cleanup still removes exact keys; recertifying an inclusion
can cancel its queue position. A later caller must enqueue again. No fairness
under arbitrary recertification schedules, arbitrary-load availability or
per-action latency bound is claimed.

## Truthful diagnostic ownership

New opt-in `automatic_status_fifo_version=1` distinguishes this dispatch from
the frozen legacy captures. Selection is 1 for queued/no budget, 2 for own,
3 for another owner and 4 for missing owner/no transport. Selected market/action
IDs are recorded separately. An OTHER upgrade receives no caller timing pointer:
its legacy `outcome_upgrade_*` fields cannot be attributed to the caller's ID.
No emitting inner span is created while client work is held.

Legacy decision 9 retains its real budget-refusal meaning. New decision 19
means this eligible caller yielded to a FIFO selection, not an HTTP attempt,
execution result or nonexecution proof. The
[old bounded diagnostic](b3-flowmesh-outcome-slice-20261016.md) remains pinned to
its measured old native source. Its strict helper was not silently changed to
interpret this new vocabulary; a future capture needs a disclosed successor
profile/helper. No old trace or reason is reinterpreted.

## Deterministic test-only boundary

`node::test::StageFlowMeshPollForTest` has no RPC, operator flag or node caller.
It refuses non-regtest chains and scalar times outside real-now plus 5–15 minutes.
It uses the existing work owner and scheduler, advancing monotonically and
optionally spending its two existing burst tokens. Ordinary `Advance` therefore
performs its normal refill and rolling-window expiry; no custom refill,
replacement/reset scheduler, refund, global clock override, persistence or
original mutation is introduced. Test +10-minute staging prevents incidental
TLS/fsync elapsed time from manufacturing an extra token. BEFORE is explicitly
instrumented, not byte-identical production.

Both TLS leaves use fresh generated fixture originals, actual native dispatch,
genuine threshold-BLS inclusion/evidence and exact request counters. They send
zero economic submit requests. The fixed-membership/indexed-chain fixture is
not a full B3 validation, live-wallet, Qt or WAN qualification.

## Actual before and after evidence

The first source draft's standalone `+` was caught before compilation. Actual
R3 build exited **2**, with zero tests: a new assertion treated throwing void
`CDBWrapper::Write` as Boolean. R4 corrected only that API use and added the
requested OTHER-included branch. All draft/R3 files and build logs remain.

R4 built successfully. Its primary native child exited **201**: 742 assertions
passed, 10 failed before a later fatal assertion. Crucially, the actual tuple
`[hot action read, older action read, evidence, submit, waited submit]` was
**`[1,0,1,0,0]`**, rather than `[1,1,0,0,0]`: the included caller overtook older
work. Later aborted controls are not described as executed successfully.

The R4 OTHER test stopped before its intended call because native fixture
logging enables BENCH by default. Its 725-pass/two-failure child result and
wrapper exit 1 were retained, not counted as the desired counterexample.
R5 adds only a local logger scope to that new test, preserving its strict
BENCH-off precondition. It saves/restores exact mask/levels after all client,
server and restarted owners drain, including assertion/exception unwinds.
The primary test body remains byte-identical, extraction SHA256
`8103edc31e7330430d774de794b2d7bd9192d1886c851301e18b919e9bfb91b2`.

R5 BEFORE built successfully. Its OTHER child exited **201**: 736 assertions
passed, seven failed before a later fatal assertion; the intended failure
message and request tuple show the newer caller serviced itself rather than
the older included outcome. Its pure scheduler companion passed 1,026 assertions
with child 0 even BEFORE—it checks existing bounds, not the integration defect.

After the correction, the five-target build exited 0, then all three leaves
passed once with actual child 0 and zero assertion failures/warnings:

- Included caller yields to older unresolved work: 782 assertions.
- Unresolved caller services OTHER included outcome, preserves identity and
  retention-only restart: 766 assertions.
- Existing scheduler coalescing/removal/no-refund bounds: 1,026 assertions.

The expected-red collector's 0 means it captured predetermined counterevidence,
not that its native child passed. Actual child exits/XML are retained separately.
No assertion was inverted, skipped or weakened to make the repair pass.

## Final local qualification

The exact v10 host selection subsequently passed **500 cases in 28 suites**,
all actual children exit 0, no selected skips, 227.968 seconds. Ordinary
AppleClang 17 Release C++20, Apple M4 Max/128 GiB, macOS 26.5.2; the linked
test seam is disclosed and crash-test macros are off. Complete Qt trading
helper passed 82 cases with no skips (Qt-reported 93 ms), and workspace passed
362 with three unchanged opt-in visual skips (Qt-reported 47.541 seconds).
Both actual Qt children exited 0. Functional Qt/native runs overlapped; these
elapsed times are not a controlled trading-performance comparison.

The v10 stdlib runner controls passed 56 tests in root's integrated execution
(exit 0, 0.189 seconds) and one separate non-author private execution (exit 0,
0.201 seconds). They are not combined with each other or the native count.
All previous suite mappings/requirements remain. No remote CI, Linux,
DEBUG_LOCKORDER, TSan, Windows, WAN or full-V2 qualification occurred. The
unchanged isolated models had separately passed 446 cases; they were not rerun
or represented as qualification of this native integration. Source hashes
below were frozen for execution and remain unchanged by the documentation.

## Frozen source and executable identities

Final source SHA256:

- Client: `f6610edb8a3555e29af11d414563b2d814ae9e1d96feba0284a41a1d25a54689`.
- Header/test seam: `45caf67305857c847f458f66bec8cef82f86012bfa595e38ca5d4029c51a49a8`.
- Scheduler header: `8996fc77d9916e3de5bf81e226b0210dc292d62aad3aa0432a729b4236af9c23`.
- TLS tests: `d393753dc16de06376bff751bf3529b0f83673f8df4c1c02a764f02cc297b527`.
- Pure poll tests: `e39a8cefb26c678e2d63057f2035f520db09b6c1e127fc7c7585493a60715e13`.

Instrumented BEFORE client: `b9b1940dcfc6737c0b68b4d654fa867107d37d31c0997fa19739948e994d849e`.
R4 executable: `6f8313791e1629345f43231b1c37bf00986f29f0dd0175a4de28ee4ff8db053f`.
R5 BEFORE executable: `daa3b7eeac4306ba959eb1ced1d4a342147a5e29cb5e3483b54e50f91b9ce871`.
AFTER executable: `52a27a3d7bdc21d5c365e6dd86758486605cb2e2c0df0b3f003704727d3a0fe1`.

After an ordinary matching native build, reproduce the complete affected suites:

```sh
./build/bin/test_bitcoin \
  --run_test=flowmesh_client_outcome_evidence_tests,flowmesh_client_poll_tests \
  --report_level=short --log_level=error
```

The [strict v10 selection](b3-flowmesh-native-linux-guardrails-20261016.md)
additionally requires all three exact names without replacing earlier cases.
Separate non-author source review accepted the frozen fairness implementation,
diagnostic identity and R5 fixture correction; that reviewer ran no native tests.
Root performed the actual builds/runs. Passing these functional controls is
not an independent audit of the whole tree, Linux/TSan, old-backup rollback,
physical power loss, future consensus/economics, WAN, 200 ms or release approval.
Generated keys, journals, databases and raw traces remain private. No push,
live operation, activation or deployment.

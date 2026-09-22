# Milestone 3A successor: bounded headers and candidate-local aggregation

Status: implementation and bounded model qualification, **external re-review
pending**. No production integration or deployment approval.

Reviewed parent: [`00d41abc85c3fc15d6c4191bcafd699cc16b7836`](https://github.com/B3-Coin/B3-CoinV2/commit/00d41abc85c3fc15d6c4191bcafd699cc16b7836).
The working branch initially matched that revision exactly. R1 atomic
view-change persistence, R2 vote retention and R3 timer eligibility are
preserved. Their report and earlier failure evidence are not rewritten.

## Cause and narrow correction

`Replica._proposal` retained a verified header before rejecting a conflicting
proposal. Headers had no admission-time capacity policy. The former
`_aggregate()` loop subsequently examined every retained header. Expiring
references and bounding bodies did not bound the separate header map.

The [frozen diagnostic](evidence/header-repair/README.md) reproduces 100, 200
and 300 inputs retaining 100, 200 and 300 headers; bodies stop at 256. A
separately labelled diagnostic aggregate invocation visits 100/200/300 headers.
The clock advances 31 ticks per input but **tick servicing is withheld**.
This is a delayed-timer resource counterexample, not permanent failure under
fair-timer assumptions. Diagnostic calls are not claimed to occur after every
conflicting ingress in the old implementation.

The successor changes only the isolated model's resource handling and
aggregation triggers. Proposal context, body and NEW_VIEW checks precede
retention where applicable. Accepting a header is never permission to issue a
second vote. Conversely, a valid competing PreparedQC must remain actionable
even if this node accepted a different naked proposal.

## Retention and protection

The [agreement TEST profile /4](../flowmesh_v2_agreement/TEST_PROFILE.json)
allows at most **32 cached headers and 67,108,864 accounted bytes (64 MiB)**.
Each entry costs the canonical JSON length of
`{"slot": [view, ValueId], "signed": signed_header}`. Costs are summed per
entry. This is serialized-byte accounting, **not Python heap/RSS accounting**;
it does not include bodies, certificates, durable history or audit storage.
Existing separate proof, body, pending, history and storage bounds still apply.

Protection is derived from exact current-instance `(view, ValueId)` slots in
durable accepted proposals, prepared proofs, signing intents, validated
NEW_VIEWs and a durable decision. Internally pending fully verified
PreparedQC/CommitQC recovery also protects its exact header. Peer priority
labels, naked claims, and arbitrary pending proposals do not grant protection.

On every admission, expired disposable entries are reclaimed even if timers
have not run. If necessary, oldest unprotected entries are evicted to fit both
limits. Cleanup removes their exact disposable vote buckets, announcement
marker and stale associated references/requests/pending ordinary traffic.
It does not delete complete recovery proofs, local offers, durable records,
issued-signature history or retained bodies. Bodies keep their existing policy.

If the incoming header cannot fit, `HEADER_BYTES_PRESSURE` or
`PROTECTED_HEADER_CAPACITY` is reported with `kind=HEADER_CACHE, admitted=false`.
This is an explicit cache refusal, not a permanent signing halt. Fully
validated durable evidence remains usable through exact current-record
lookups even when its redundant cache copy is refused. Restart timers rely on
the durable pending work, not on successful cache restoration.

The [disk TEST profile /3](TEST_PROFILE.json) references agreement /4. SQLite
schema and codec remain version 1. Changing the synthetic profile changes the
test configuration identity: this is not a migration of earlier test stores
or reinterpretation of old signed messages.

## Work bounds and recovery

Each relevant vote or header triggers `_aggregate((view, ValueId), phase)`:
one candidate lookup, at most two phase checks, at most `2*N` collected-vote
entries and two constructed proofs. A COMMIT event examines commitment only.
Lookup uses at most one header-cache access and three exact durable accesses.
Complete nested-proof validation and application have their existing separate
bounds; the counts above are **not total CPU or latency measurements**.

There is no dirty-candidate backlog or periodic aggregation scan. Header
maintenance inspects at most four bounded header-table passes (128 entries at
the declared cap), bounded current-record protection entries, and the pending
recovery table (32). At most two cleanup passes inspect each associated
reference/request/pending table (each capped at 32). Removing one header
checks two vote slots directly. Work counters expose these operations. No
maintenance path scans lifetime signature or committed-decision history.

Votes may precede their header. Header arrival evaluates the already received
votes; complete PREPARED arrival also evaluates already received COMMITs.
Evicted candidates can return through existing exact proposal/DATA retries,
valid complete proofs and requested missing-body recovery. Historical
decisions remain discoverable through existing STATUS/GET_CERT and committed
storage. No new wire type, timeout, quorum or voting/unlock rule is introduced.

## Qualification and reproducibility

Run from a clean checkout of the published successor using Python 3.14:

```sh
python3.14 -B ci/run_flowmesh_models.py
```

The dedicated GitHub workflow runs that same bounded command on
`flowmeshV2-dev`, with read-only repository permissions and no deployment.
It retains discovery floors, independent-checker negative controls, fixed
seeds, full issued-signature history and failure-on-skip behavior.

New regressions are in
[test_header_bounds.py](../flowmesh_v2_agreement/test_header_bounds.py) and
[test_header_recovery_disk.py](test_header_recovery_disk.py). They test the
delayed-timer input pattern, count/byte pressure, expiry/admission cycles,
protected evidence, real competing quorums from allowed synthetic signers,
votes before headers, eviction followed by missing-data recovery, same-store
reopen and exactly-once application. Count/byte overrides are explicitly
cache-only synthetic fixture parameters, not changes to quorum or economics.

The progress assertion is separate from resource/safety assertions: after
the finite hostile prefix, ordinary timer service and fair delivery resume;
all honest replicas must apply the same decision exactly once with **no
fresh client request and no new or retransmitted OFFER**. Original accepted
instructions, issued votes, preparation and overdue deadlines remain intact.

The complete runner now discovers **332 tests**: 113 accounting, 142 agreement
(including 10 new header regressions), 69 storage (including 5 new header
recovery regressions), and 8 runner checks. Final committed-tree execution and
the exact remote CI result are recorded in the milestone handoff; a file count
or working-tree development run alone is not that qualification.

The matching original ingress schedule now retains 2 headers / 1,344 accounted
entry bytes at 100, 200 and 300 inputs, with one actual keyed lookup. For direct
comparison with the baseline's different whole-list serialization, that list
measures 1,349 bytes rather than 67,401 / 134,801 / 202,201. The body counts are
unchanged at 100 / 200 / 256. The exact schedule has no initial PreparedQC;
the separate recovery regression starts with a genuine prepared obligation.
Neither measurement substitutes for the other.

A separate 300-input same-clock pressure test reaches the default 32-header
cap (21,504 accounted bytes), then evicts disposable headers without a timer.
Its reference-per-slot fixture limit is explicitly raised to expose this
second capacity path; the normal profile is not relaxed. Artificially tiny
128-byte and one-header caches additionally refuse cache copies while durable
evidence still supports normal signing/aggregation and same-store recovery.

During test development, an immediate-application assertion after reopening
failed: the node retained a valid competing preparation but its body was only
volatile. The fixture now asserts that COMMIT-only input does not allocate an
unauthenticated body request; retransmission of the **same existing PREPARED**
proof permits normal GET/DATA recovery, then the existing COMMIT set applies
once. No rule was weakened and no new action was introduced. The failed
development run is preserved separately from final qualification. An earlier
combined focused invocation also omitted the agreement import path; correcting
the launcher was not an implementation repair.

New disk header scenarios use actual SQLite close/reopen with same-process
scheduling. The full runner also reruns the earlier separate-child SIGKILL
and I/O-failure cases and records their actual exits. These categories must
not be conflated. Neither establishes machine power-loss durability.

## Remaining limits

Fixed synthetic membership/authentication/anchors, finite test bounds and
eventually timely fair delivery remain assumptions. This is not arbitrary-load
performance, WAN/200 ms qualification, real BLS/B3/bridge compatibility, live
V1 lock recovery, changing membership, mainnet bootstrap, futures risk or
production storage/fencing. A coherent old backup still requires an external
trusted freshness commitment to detect rollback. External review must inspect
this successor and its interactions, not substitute CI for an audit.

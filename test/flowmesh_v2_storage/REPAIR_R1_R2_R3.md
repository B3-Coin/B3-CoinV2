# Milestone 3A: response to the three reproduced review findings

Branch: `flowmeshV2-dev`. Frozen reviewed revision:
[`1d022dbf0da63636d6d43650d3864222761497a1`](https://github.com/B3-Coin/B3-CoinV2/commit/1d022dbf0da63636d6d43650d3864222761497a1).
The original Milestone 1, R1, agreement and storage histories are preserved.
This is a successor correction, not a rewrite of the reviewed evidence.

**Status: REPAIRED + AUTOMATED TESTS PASSED.
EXTERNAL RE-REVIEW PENDING.** Automated checks are not independent review.
The final committed tree must pass the runner before push, and hosted CI must
be observed for that exact remote revision; the handoff reports both separately.
No production node, live wallet, real signing key, bridge, activation, economic
rule or next milestone is involved.

## Finding response

| Finding | Cause and smallest repair | Frozen failing-before result | Focused repaired result |
|---|---|---|---|
| R1: lost view-change report obligation | `Replica.change_view` committed CHANGING/view before a separate report intent. It now commits both the new view and exact VIEW_CHANGE intent in one existing durable transaction. Normal intent, signature and publication checks remain in force. | With N=4, seat3 silent, SIGKILL after node0's SQL view-entry commit left three honest nodes at sequence0/view1/CHANGING through tick232. No node0 report intent existed. No-kill control settled tick36. | Same store reopens with the report obligation; all three honest nodes apply exactly once without any post-reopen OFFER/client request. Surrounding persistence/signature/publication cuts and exact signed-object replay pass. |
| R2: unowned received-vote buckets | Reference leases expired but received PREPARE/COMMIT buckets were never reclaimed. `AdmissionMixin` now ties each disposable bucket to an exact live reference or protected current-record obligation, expires disposable entries and caps the whole received table at64 buckets. | 150 expiry cycles retained300 buckets with only2 references/requests/pending entries, no durable growth and no halt. | 150/300/450 cycles retain2 buckets, peak cleanup scan2; independent audit retains600/1,200/1,800 signatures. Delayed votes, prepared/commit/NEW_VIEW evidence, restart, pressure rejection and subsequent agreement pass. |
| R3: request bypass of timer prerequisite | `_offer` called a timer helper that did not check mode/report eligibility. Central `Replica._timer` now requires ACTIVE, or CHANGING plus q verified reports for the current target. | Remote OFFER with zero reports created deadline60, then advanced to view2. | Remote/local OFFER, retry and restart cannot start a forbidden timer. Genuine q same-target reports do start the existing timer. ACTIVE and sparse idle-nonleader progress remain tested. |

Source:
[replica](../flowmesh_v2_agreement/fm_replica.py),
[admission](../flowmesh_v2_agreement/fm_admission.py),
[process inspection](fm_process_harness.py).
The latter only exposes bounded receipt/cleanup observations to the parent
test harness; it does not alter voting or platform dispatch.

The exact original counterexamples, controls, provenance and portable frozen
launchers are preserved in [evidence/m3a-repair](evidence/m3a-repair/README.md).
Their successful launcher exits mean the baseline failures reproduced; they
are not passing-after tests.

## Revised durability invariant and crash states

Atomic view entry stores `view=target`, `mode=CHANGING`, and
`intents[("VIEW_CHANGE", target)]` containing the exact current instance,
sender, target, highest locally retained preparation and null decision.
Preparation evidence already resides in the same protected durable record.
No proof is manufactured and no old vote, QC or lock is removed.

| Cut | What reopening may use | Next permitted action |
|---|---|---|
| Before view-entry SQL commit | Old view and all its existing obligations | Existing recovery/timer/report rules; no invented target report |
| After view-entry commit, before report signature | New view **and exact report intent**, with prior preparation evidence | `_resume_intents` resumes the same intent through ordinary persist-before-sign checks |
| During the ordinary report-intent transaction | Atomic view-entry intent still survives even if this repeat transaction aborts | Resume identical intent, never substitute another report |
| Signature transaction committed before publication | Exact signed report is in the store | Retransmit that object; no new instruction/signature |
| After publication, before certificate | The same issued obligation remains binding | Retry the exact report; collect normal evidence |

The ordinary intent transaction is intentionally retained after atomic view
entry: it keeps the existing evidence-before-first-signature path and independent
checker events, rather than fabricating a historical pre-sign event on reopen.
The model is single-threaded: no incoming event runs between synchronous
view-entry and report-signing steps; restart resumes its intent before ordinary
new ingress. Failure makes the disk adapter unavailable and reopening reconciles
the actual store, as before.

This repairs the new transition, not arbitrary old test stores. The successor
agreement profile has a different synthetic configuration identity; no migration
or reinterpretation of frozen `/2` signatures/stores is implemented. Physical
power-loss and device write-cache behavior remain unqualified.

## Revised received-vote ownership and work limits

The new versioned [agreement TEST profile](../flowmesh_v2_agreement/TEST_PROFILE.json)
is `flowmesh-v2-single-sequence-pbft-test/3`. The
[disk TEST profile](TEST_PROFILE.json) is `flowmesh-v2-disk-recovery-test/2`;
SQLite schema1 and codec1 are unchanged. The original profiles remain in frozen
history. This is a test-policy change, not a mainnet default or a quorum change.

- A bucket is `(view, exact ValueId, PREPARE-or-COMMIT)`, containing at most N
  verified sender entries. The per-replica cap is64 buckets, hence at most64N
  entries (256 for N4;448 for N7).
- A disposable bucket requires a live matching current-instance reference at
  that exact view. A stale cached proposal header alone cannot protect it.
- An accepted proposal, locally persisted signing intent, valid prepared QC,
  or accepted NEW_VIEW protects its exact current-record `(view, ValueId)`.
  Durable records and independent all-issued-signature audit history are never
  evicted by cache policy.
- Expiry runs on admission and periodic retry. Each vote cleanup scans at most64
  buckets, then removes at most64 identified disposable entries. It walks only
  the current record's bounded obligations, never accumulated durable history.
  At most8V obligation entries are inspected under the declared fault bound:
  accepted V, prepared V, intents5V and NEW_VIEW V, with V=8.
- Periodic admission cleanup additionally scans the existing at-most32 requests
  and32 references. These bounded cleanup operations are separate from the
  existing delivery budget of96 objects,4 records,256 scheduled messages and
  32MiB per retry; no claim hides cleanup inside that scheduler counter.
- At full capacity an incoming protected bucket can displace a disposable
  bucket. Otherwise admission returns `RECEIVED_VOTE_PRESSURE`; it does not
  halt signing, delete protected state or enlarge campaign limits.
- Tests deliberately use cap2 for pressure-only cases, constructing the
  synthetic configuration under that parameter. This is explicitly not the
  profile's default64 or a voting/economic parameter.

The ≤f assumption permits only one valid prepared value per view, although a
local minority accepted value may differ. Accepted/intended/NEW_VIEW and valid
prepared obligations therefore fit conservatively within4V=32 phase buckets.
Review should verify this capacity argument and eviction precedence, not merely
the chosen64 constant. Even unexpected all-protected pressure refuses incoming
cache admission; it never authorizes discarding a durable obligation.

Separate bounded diagnostic observation: with timer servicing deliberately
withheld, 300 successive valid referenced conflicting bodies can retain300
proposal headers while received votes remain0. This is not the exact R2
fair-tick schedule and has not been repaired here. The received-vote/cleanup
bound is **not** a global heap or `_aggregate` header-scan bound. Review any
broader hostile-ingress/worker-scheduling guarantee separately; do not silently
promote these cache tests into that guarantee.

## Timer eligibility and interaction coverage

All existing timer entry paths (OFFER/local request, queued recovery, restart,
accepted proposal, report reception, accepted NEW_VIEW) use the central
predicate. Only already verified same-target distinct reports count. f+1 view
synchronization, q report threshold, 30/240 timeout parameters and10-tick retries
are unchanged. Duplicate data/reports cannot postpone an existing deadline.

The actual-process interaction matrix retains R1's original fault assumptions:
N4, seat3 silent Byzantine, three responsive honest replicas and eventual timely
honest delivery. Independent checks distinguish:

1. **SAFETY:** checker examines all issued signatures, including hidden ones.
2. **PROGRESS:** all three honest replicas apply the same original batch once.
3. **RESOURCE:** received buckets and cleanup work remain bounded; junk cannot
   change protected records or the trusted durable witness.
4. **TIMER:** neither unrelated OFFER nor junk votes supplies missing reports.

A: no new client traffic and no fresh **or retransmitted** OFFER after reopening.
B: an additional valid OFFER is admitted but cannot start a CHANGING timer.
C:300 unknown-value votes signed by seat3 are refused during recovery; delayed
real votes for the protected candidate remain usable and agreement completes.
Seat3 cannot forge proposer0's reference, so C is not falsely described as
R2's accepted-reference churn. That exact churn is separately executed with
Byzantine proposer0 through memory and real SQLite adapters.

## Tests and reproduction

From the repository root, Python3.14 on the supported POSIX test host:

```sh
python3.14 -B ci/run_flowmesh_models.py
```

The runner discovers all `test_*.py` and rejects omissions, skips, xfails and
duplicates. Floors now include these25 new test methods (subcases are not
additional methods):

| Added file | Methods | Distinct coverage |
|---|---:|---|
| [test_vote_retention.py](../flowmesh_v2_agreement/test_vote_retention.py) |9|150/300/450 cycles, expiry, delayed votes, body recovery, stale headers, protected evidence, pressure and post-flood progress|
| [test_timer_eligibility.py](../flowmesh_v2_agreement/test_timer_eligibility.py) |7|All negative callers, wrong-target reports, genuine quorum N4/N7, ACTIVE and sparse failed-leader recovery|
| [test_view_transition_repair.py](test_view_transition_repair.py) |6|11 separate-child scenarios,10 intentional SIGKILLs,44 clean child exits; exact cuts and A/B/C|
| [test_admission_repair_disk.py](test_admission_repair_disk.py) |3|Exact churn with unchanged SQLite head; exclusive same-directory close/reopen; subsequent agreement; negative/positive timer evidence|

Complete discovery and execution was317 methods:113 accounting,132 agreement,
8 runner and64 storage. Focused runs are subsets, not additional unique tests.
Existing predetermined campaigns remain unchanged: agreement45312–45319;
storage31031/31032; accounting0xA110–0xA113 and0xF10A2026 (permutation seed
XOR0xD15EA5E) remain in its frozen tests. New repair
schedules are fixed and contain no randomized seed selection.

Development checks executed:132 agreement tests;6 process-repair methods
with11 scenarios;3 disk-admission methods. The initial process test had an import
ordering error, corrected before the successful run; no assertion was removed
or weakened. Full pre-publication runner on implementation commit
`a5c6a0559ba4452ff288575ea093c36dbb24b2c0` plus the new discovery floors passed
all317, exit0: runner8 (0.010s), accounting113 (62.456s), agreement132 (75.191s),
storage64 (44.345s), Python3.14.6/macOS26.5.2 arm64. No failures, skips or
expected failures. Log SHA256:
`0c16ef0930c2035ae8ff2cb13dacace8139b44749e5d92b8f960ec8d3b465b74`.
The added process matrix recorded10 expected kill exits(-9),44 clean exits(0),
and no remaining child. Focused repeats are not added to the317 distinct tests.
The final committed-tree rerun and actual hosted result are recorded at handoff
and in the exact revision's GitHub Actions run, not inferred from this earlier
working-tree run.

## External re-review and unqualified requirements

Review the atomic transition/content preservation; received-vote ownership,
capacity argument and pressure precedence; periodic cleanup workload; central
timer predicate; and their new process/disk regressions against the frozen
counterexamples. Passing CI does not close this external re-review gate.

Authentication, membership, anchors, custody, clock and delivery remain
synthetic and fixed. Supported progress assumes a responsive quorum, payload
availability, eventual timely delivery and fair timer servicing within finite
campaign bounds. No live V1 split-lock recovery, real BLS, genuine B3 anchors,
membership changes, PoS V2, bridge verifier, futures risk, WAN/200ms performance,
network filesystem or machine power-loss qualification is claimed. Known
rollback still requires the external trusted witness; a coherently restored
old backup without it remains undetectable.

No source prerequisite, evidence history or production branch is replaced.
Publication is to the same V2 branch for review only, without a PR, release,
deployment or Milestone3B implementation.

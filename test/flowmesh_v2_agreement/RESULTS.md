# Milestone 2 evidence and limitations

**Historical frozen-stage record below.** The later authorized publication
review, new counterexample and successor correction are documented separately
in [POST_FIX_REVIEW.md](POST_FIX_REVIEW.md). Earlier failures/review limits below
are retained, not silently relabelled as passes or overwritten.

Status: isolated model closed for owner review; no production qualification.
Exact final revision is the local milestone commit containing this document.
Run commands are in [README.md](README.md). Frozen accounting and design files
have no changes relative to the Milestone 1 parent.

## Publication and scope

At handoff recovery, read-only GitHub checks confirmed Milestone 1's remote
branch `model/flowmesh-v2-accounting-test1` at
`9d4fd53dc7d58d397680fd64065663703cbbdd17`. PR #76 was closed unmerged,
following the owner's later branch-only/no-PR instruction. Nothing was
republished. Milestone 2 consists only of local commits for review.

## Actual qualification

The final clean-tree rerun is recorded in the completion handoff. Before the
last additional two-anchor competition case, the complete agreement suite
passed **78 unittest methods in 52.180 seconds**; the unchanged accounting
suite passed **113 methods in 65.468 seconds** on Python 3.14.6. These durations
are host execution times for the Python tests, not simulated or WAN latency.

The suite includes subcases and campaigns; method count is not schedule count:

- 48 enumerated N=4 initial split schedules: all 8 A/B assignments to the
  three honest receivers × all 6 first-proposal delivery permutations. Each
  checks prefixes, sends opposite proposals, and finishes with a fair tail.
- 16 predetermined randomized campaigns: seeds 45312–45319 for each N=4/7;
  maximum randomized delay 8 ticks, 15% message-drop decision and 20%
  duplicate decision, three 12-tick fault-prefix segments, one supported
  honest crash/restart, then reliable tails of at most 350 and 150 ticks.
  Faulty leader withholding is present on alternating seeds; it does not
  replace the separately targeted Byzantine equivocation tests.
- 16 PREPARE/COMMIT crash schedules: two rosters × two phases × before intent,
  after intent, after durable signature, after publication. Four more
  schedules cover durable decision/application boundaries. Additional tests
  target missing evidence, sparse offered work and cached-header restoration.
- Four omitted-local-proof schedules: two rosters × proof learned before or
  after the already immutable VIEW_CHANGE report. No artificial local veto.
- Hidden commit quorums for both rosters, delivered solely to the Byzantine
  collector before reveal; a separate check counts quorums made entirely of
  unpublished signatures when honest signers crash after persistence.
- A sequence of deposits, reservation, partial fill, cancellation and explicit
  spot/futures transfers applies the unchanged accounting batch exactly once.

This exhausts only the declared 48-case finite space, not arbitrary schedules.
Random seeds are reproducible coverage, not a probabilistic safety proof.

## Required scenario map

| Requirement | Evidence / outcome |
| --- | --- |
| A: successive decisions | Both rosters, four sequences; common roots and exact parent continuity |
| B: unavailable leader | Honest next view completes under fair delivery |
| C: equivocation/split preliminary votes | Recover with NEW_VIEW; retain old obligations |
| D: anchors 855500/855501 | Same actions/root but distinct ValueIds; private tips never rewrite a received body; explicit competing-anchor case |
| E: partial preparation | Highest reported QC carried; lone omitted QC cannot veto; late proof never rewrites a signed report |
| F: hidden COMMIT quorum | All issued votes counted before any local decision; incompatible next-view choice rejected |
| G: late certificate | Applies after timeout/restart; no replacement decision |
| H/I: 2/2 and 4/3 partitions | No possible commit quorum; heal and recover under fair delivery |
| J: 5/2 partition | Five compatible participating seats progress; one withholding seat among those five prevents progress |
| K: far-future claim | One Byzantine claim does not advance/reset honest state; authenticated justified evidence handled separately |
| L/M/N: crash boundaries | No pre-record publication; original intent/signature survives; decision/application replay once |
| O: known rollback | Permanent model signing fence; exact old messages/read-only certificate catch-up remain possible |
| P: malformed/replayed context | No duplicate weight; bad domains/phases/sets/views refuse; resources explicitly bounded |
| Q: absent payload/evidence | Request exact data, no invented state or blind vote |
| R: last sparse operation | Completes without another trade, including a sole submission to an idle nonleader |

## Reproduced failures, preserved and repaired

The pre-fix snapshots remain in history; no squash or signature/journal reset.
[evidence](evidence/) contains actual synthetic traces, issued messages and
failure output. The regression names below are runnable against those
revisions with `replay_case.py --source CHECKOUT --revision REV --case NAME
--output FILE`. The output labels expected failures as `success:false`.

| Before revision / evidence file | Minimal failing path | Repair and passing regression |
| --- | --- | --- |
| `5fc4b67`, `before-anchor.json` | Persist decision at anchor 855501 → crash → discard volatile evidence → restart throws NeedData | Defer exact existing CERT, fetch anchor evidence and resume same unapplied decision; `test_N_restart_after_decision_recovers_missing_volatile_anchor_evidence` |
| `5fc4b67`, `before-offer.json` | Sole offer retained → crash before PROPOSE intent → restart has no pending-work path | Restore current-instance offered body and timer; `test_N_sole_final_offer_survives_crash_before_proposal_intent` |
| `5fc4b67`, `before-prepared.json` | Accepted proposal/PQC durable → restart loses header → receive q COMMIT votes → no assembly | Rebuild header index from own durable accepted/prepared evidence; `test_N_restarted_prepared_replica_aggregates_commit_votes_without_new_proposal` |
| `5fc4b67`, `before-new-view.json` | Accept NV-A → Byzantine leader sends proposal with NV-B, equal value but different report subset → honest PREPARE | Enforce exact accepted NV; both bundle orders tested in `test_exact_new_view_cannot_be_substituted_after_acceptance` |
| `5fc4b67`, `before-pending.json` / `before-forged.json` | Future-context packets fill pending cache → uncaught exhaustion; forged packets previously retained before authentication | Authenticate/validate before SIGNED defer; record durable PENDING_LIMIT without erasure; two focused tests |
| `1cb2dc7`, `before-local-limit.json` | Inject exhausted signature-audit bound → local offer throws outside recovery boundary | Local offer/restart record explicit halt, retaining intent; `test_local_resource_exhaustion_preserves_intent_and_halts` |
| `1cb2dc7`, `before-nonleader.json` | Only idle nonleader receives final offer → one timeout report forever | Bound exact OFFER dissemination/retry; `test_sparse_offer_to_nonleader_reaches_agreement_without_next_request` |

No attainable conflicting final commitments were found in these scenarios.
That is not a proof that none exist in every schedule or implementation.

The independent checker also had two demonstrated coverage gaps, using
deliberately invalid implementation/state mutations as **negative controls**:
late insertion of a QC could hide an earlier unsafe COMMIT, and an unrelated
higher anchor escaped its original height-only check. It now checks historical
signing guards and intent→signature-persistence→publication order, plus its own
multi-height ancestry walk. Six additional checker methods exercise these
controls and valid history after all caches restart. These are oracle repairs,
not claims that a real adversary could directly mutate an honest journal.

Intermediate non-protocol failures are also retained in the working record:
tests initially ran before their checker dependency existed; one crash test
assumed a restarted node must vote even when another quorum could finish
first, corrected by delivering its genuine retained trigger; enabling OFFER
forwarding made the invalid-vote test accidentally deliver valid work to its
primary. That test now deliberately drops OFFER while exercising unchanged
bad-vote assertions. None is presented as an agreement safety counterexample.

## Review status

A fresh internal AI reviewer, separate from implementation helpers, inspected
the profile, transitions, checker and adversarial cases. Its concrete findings
are reflected above. The parent reproduced them and ran repaired regressions.
A requested follow-up of only the fixes did not execute because the agent tool
refused that follow-up; no independent post-fix approval is claimed. This is
not an external security audit. Stop at this frozen milestone for owner review.

## Bounds and safe stops

Configured views 0–7, at most 32 decisions, 20,000 unique issued messages,
512 inbox/pending entries, 256 body/anchor objects, 2 MiB proof bytes,
20,000 proof-work units, depth 32, 60,000 queued/delivered events and 100,000
trace events. Harness work is capped at 120,000 run steps. Object/history
retention does not prune signing obligations to create space.

Replica resource failures stop new signing with a reason. Global harness
event/trace/work exhaustion aborts the bounded run with its explicit reason,
not a pass. No promise extends beyond these bounds. Timeouts are 30 ticks,
doubling to 240; retry is 10 ticks, not a tuned millisecond performance value.

Progress also stops with insufficient participating quorum, unavailable exact
evidence, unresolved freshness, or conflicting valid proofs. An online peer
is not a vote. A valid old backup restored without detectable evidence is
outside the crash guarantee. Every signed obligation remains retained.

## Not established

No live V1 recovery, production cryptography/storage or crash/power-loss
qualification; no production network changes; no 200 ms or WAN benchmark.
No PoS V2, dynamic membership, bootstrapping/cutover authority, bridge verifier
equivalence or authenticated custody. No futures margin/oracle/liquidation
system. No stable-fee dust redesign: Milestone 1's disclosed provisional rules
remain unchanged. V1 history, live keys and market balances were untouched.

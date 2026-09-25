# P2-FV message/restart research gate

This finite TEST model does not change native consensus, earlier agreement,
SQLite, accounting, or wallets. **161 ms is a simulation**, not these tests'
result. The earlier native single-fill control was 445.572 ms; neither is
a measured latency of this model.

## Reproduce

From repository root, Python 3.14 and standard library only:

```sh
python3.14 -B ci/run_flowmesh_models.py --suite fastpath_research
python3.14 -B ci/run_flowmesh_models.py
```

The original evidence-only probe and its `TEST_PROFILE.json` are preserved.
The new profile is [MESSAGE_PROFILE.json](MESSAGE_PROFILE.json), version
`p2fv-message-restart-test/1`. Choices below are proposed TEST rules, not
implicit owner ratification of production economics or consensus.

## Exact rules modelled

One abstract executed value x or y, fixed synthetic N=4/5/7, views 0..2.
An application is one atomic abstract marker, **not** an accounting batch.
f=floor((N−1)/3), Q=N−f, t=min(f,floor((N−3f+1)/2)), F=N−t.
Votes bind instance, phase, view, value. Leader=view mod N. There is one
sequence only; this establishes nothing about multi-microblock tenure.

* Proposal includes leader's PREPARE: one vote, not an extra vote.
* FAST is F distinct view-zero PREPAREs including original leader zero.
* PC is Q distinct matching PREPAREs. SLOW is Q matching COMMITs.
* COMMIT requires current ACTIVE view, accepted matching proposal, exact
  NEW_VIEW where applicable, and PC. It is enabled in later views and at
  view zero only when F>Q. CHANGING is not ACTIVE.
* Exact old signatures may be retransmitted. No new old-view signature is
  authorized after advancing. Complete decisions survive timeouts and
  conflicting local acceptance; an individual PC is not a local veto.

REPORT is an immutable snapshot at signing: original own view-zero vote
plus original leader vote, highest PC, and known decision. NEW_VIEW contains
exactly Q reports; selection uses only its transmitted evidence:

1. A reported decision is applied instead of creating NEW_VIEW.
2. Highest PC wins; conflicting equal-highest PCs are a safety refusal.
3. With old-leader equivocation, a value with at least f+t view-zero reports
   wins (lexical tie-break); no qualifying value allows either valid value.
4. Without equivocation, preserve the sole reported value, or allow either
   valid value if none is reported.

Two disclosed clarifications: NEW_VIEW carries the pair of original
equivocating leader votes even if its source report was excluded from Q;
and proven equivocation globally excludes old leader zero from Q, including
the PC branch. The original report's precedence on the latter was ambiguous.
The prior evidence-only counterexample is retained, not rewritten away.

## Durability and oracle boundary

Ideal authentication separates computed from published objects. An
unpublished signature is unavailable to the adversary. A remote signing
service exposing undurable signatures would violate that assumption.

One atomic durable-memory operation stores exact signed object plus local
justification and view advancement before publication. Transferred PCs
carry votes, not recursive NEW_VIEW trees; local guards retain justification.
Crash drops volatile state only. Restart retransmits identical objects.
Missing indispensable local evidence fences the replica. Missing remote
bodies wait for ordinary NEED/DATA. Neither resets signing obligations.

Decision recording precedes application. Application and completion marker
are one atomic update; duplicates do not execute twice. This is **not** real
disk, process isolation, power-loss qualification, rollback detection, or
integration with the earlier SQLite/accounting implementation.

The independent checker imports no replica helpers. It checks carried
proofs, honest publication guards, immutable original votes, monotonic
views/application counts, and hidden quorums from **all published votes**,
not just locally assembled certificates. Audit snapshots and pre-sign
observations are trusted test facts; coherent whole-history rollback is
not detectable here.

## Initial failures and separate review

`1e9d637` preserves the first implementation. Its 29 message tests had four
failing cases: one requested-body wake-up, plus three expectations stopping
before the next legitimate timeout (two REPORT crash cuts, one late PC).
These were not four demonstrated consensus failures.

`d87b225` adds immediate NEW_VIEW work when the requested body arrives.
The dedicated regression fails without it. The timing fixtures now service
the next existing timeout; no timer interval or quorum was shortened.

A separate read-only review ran all 45 tests at `d87b225` and identified:

* Malformed EMPTY/V0 wrappers could enter honest recovery evidence; a
  foreign-instance EMPTY produced replica/oracle disagreement. Canonical
  context/shape checks now reject it before retention; valid recovery still
  succeeds afterward. This was not a conflicting-decision trace.
* Deleting durable `v0` alone left original signed vote/body intact but
  allowed a false EMPTY report. Restart and oracle now cross-check the
  summary against the immutable signature and leader evidence. This was a
  deliberate inconsistent-store injection beyond normal atomic crash cuts.

Five review regressions (ten failing subcases before correction) retain
these findings. No old history or tests were discarded.

## Qualification and limits

Coverage: healthy FAST N=4/5/7; N=7 slow fallback with two absent seats;
failed/equivocating leaders; hidden FAST; late PC; votes before proposal;
body recovery; exact retry; view-change restart; decision/application cuts;
duplicate proofs; no-quorum waiting; independent checker negative controls.
Cuts: before compute, computed-undurable, durable-unpublished, published,
decision-before-apply, during atomic application, after apply.

Predetermined campaigns: 24 shuffled schedules seeds0..23, plus 12
loss-then-fair-delivery/restart schedules seeds0..11 (drop RNG seed+900).
They are bounded examples, **not** exhaustive Byzantine exploration, a
liveness theorem, or formal proof. Queue/pass guards are 20,000/5,000
messages. Retained evidence is small under this finite profile; arbitrary
traffic/resource robustness and a production codec remain unqualified.
Elapsed test runtime is not protocol latency.

Before group regtest: finish review of the recovery protocol/adversarial
state space, build a separately reviewed native regtest-only implementation
with real cryptography, execution, disk writes and client verification,
then measure real fills across separate processes with B3 advancing.
Do not distribute the current native wallet as a 161-ms V2 implementation.
No bridge/PoS V2 completion, production activation or hosting fee follows
from this gate.

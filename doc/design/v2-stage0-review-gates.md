# Stage 0 freeze and independent-review gates

Scope: documentation only, based on V1
`0b930e303e4c2c6bc28beb3bf656488b636ad49d`. Review the actual integrated
[`specification`](b3-flowmesh-v2-stage0.md), its source anchors and
[`handoff`](v2-handoff-20260920.md); this checklist is not a substitute.

## What this stage established

- Recovered local `flowmeshV2-dev` at the exact published V1 head; inspected
  registered worktrees and relevant preserved research. No V2 implementation
  or approved complete V2 spec was found in that bounded handoff search.
- Verified changed boundaries against actual source, including stale V1
  prose, native-B3-only markets, per-market state, rejected futures actions,
  exact fees/rewards and bridge acceptance details.
- Produced one integrated review draft with ten explicit owner decisions.
  Futures is release-required. No live activation parameters are selected.
- Authoring consistency checks corrected a proposed partial-fill fee bound,
  branch-dependent committee ambiguity, BFT/export high-water conflation,
  late export checks, abandoned-view signing and spot/futures withdrawal
  bypass ambiguity. These are **design draft corrections**, not production
  bug fixes or claimed live recovery.

## Newly executed checks and their limits

The proposed fee arithmetic received a bounded integer sample check:
106,964 combinations, zero failed reserve inequalities. Using integer
BigInt arithmetic, rate=50, denominator=1,000,000:

```
fee(n) = floor(n * 50 / 1,000,000)
ceilFee(n) = ceil(n * 50 / 1,000,000)
past = 0..220000 step 997
remaining = 1..240000 step 1999
fill in {0, 1, floor(remaining/2), remaining}
boundBefore = ceilFee(past + remaining) - fee(past)
paidNow = fee(past + fill) - fee(past)
boundAfter = ceilFee(past + remaining) - fee(past + fill)
assert paidNow >= 0 and paidNow <= fill
assert paidNow + boundAfter <= boundBefore
```

This is a design arithmetic check, not implemented C++ coverage, exhaustive
overflow testing, curve-reservation proof or production qualification.
For changing bounds the specification additionally requires
`fill + newRemainingMaximum <= oldRemainingMaximum`; replacements must fund
any increase. That precondition needs explicit execution tests later.

Rejected alternative: two ten-lot bids at 10,000 quote atoms per lot, each
filling one lot per batch, generate one batch-side fee atom each time. Always
assigning that atom to the same account charges ten atoms against its initial
five-atom reserve. Cumulative per-order fee is five atoms instead. Preserve
this failing counterexample when testing D6, not only the passing alternative.

Documentation scope/whitespace and publication-sensitive-string checks were
run before freezing. Existing V1 build, unit, Qt and functional results are
retained evidence in the baseline qualification report, **not rerun here**.
No compiler, node, wallet, signing runtime or production deployment was run
for this Stage 0 documentation task.

Bounded authoring consistency review is **not** the separate independent
audit session required by the owner. Protocol safety/liveness, final byte
codecs, deployed equivalence and economics are not thereby approved.

## Independent read-only audit instructions

1. Verify the frozen revision and doc-only delta against `0b930e3`. Read
   the baseline source, not merely our status or this checklist.
2. Challenge P2 across selective delivery, hidden quorums, prepared-but-not-
   decided values, omitted local preparation, view abandonment, missing
   candidate data, epoch handover and every persist/sign/publish boundary.
   Check safety and liveness separately under the exact fault model.
3. Challenge the unique B3 authority/sequence mapping across competing
   provisional branches and the independent internal/legacy/Ethereum records.
   A new internal commit does not revoke any old externally usable signature.
4. Recompute bridge digest/set/member/withdrawal vectors from source.
   Validate the separate pending deployed-runtime/source equivalence gate;
   do not claim public-manifest hashes alone prove it.
5. Check exact-AssetId solvency and cross-market double-reservation examples,
   fees through partial fills/replacement/cancel, pending withdrawal capacity,
   and migration overlap between still-spendable V1 claims and V2 credits.
6. Challenge all futures paths for implicit spot access, oracle uncertainty,
   unrealized-profit withdrawal, margin/backstop shortfall and loss allocation.
   Do not fill unresolved D7–D9 economic choices with implementation guesses.
7. Report contradictions, omitted invariants and open choices. Distinguish
   an incomplete approval choice from an internally inconsistent proposal.
   Do not edit, implement, deploy or sign during this audit.

## Separate urgent incident workstream — unchanged

No incident repair is bundled into V2 Stage 0. A repair must pin the affected
binary/release, installation evidence and isolated reproducer separately.

| Observed condition | Evidence needed / forbidden shortcut |
|---|---|
| Missing/offline quorum | Anchored roster, exact candidate and accepted votes; peers/armed count are insufficient; no threshold reduction |
| Missing data | Exact missing candidate/proof/chain object, request/admission/catch-up trace; no substitute candidate signature |
| Orphaned checkpoint signature | Durable vote identity, branch ancestry and any valid retained/newer certificates; do not infer no hidden quorum exists |
| Permanent split final locks | Conflicting signed identities, admissible quorum geometry and old claim consequences; no journal reset or arbitrary unlock |
| Chain progress but finality halted | Separate height/hash/tip progress and signer state; no “synced” from peer count alone |
| Expired Ethereum lineage | Pinned contract state/time checks; new node software cannot authorize a replacement contract or bootstrap replay |

The extra finality re-anchoring commit in the original developer checkout
was preserved but not imported or validated as a repair in this stage.

## Stop condition

Stage 0 ends with specification review. D1–D10 remain pending. The first
proposed implementation stage is the isolated shared-spot reference model
and invariant fixtures in §14, conditional on the necessary approvals.
Futures implementation, full BFT/model proof, migration and WAN qualification
remain required before a V2 release. No push, merge, release or activation
follows from freezing this documentation.

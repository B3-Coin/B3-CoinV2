# Milestone 1 — internal accounting-model review and closure

Status: **PASS WITHIN MODEL SCOPE**. Stop for review; no next-stage authorization
is inferred from this result.

Reviewed frozen baseline: `dc5682785f3d8c91972fd2f3b8502fd30be1baa7`.
R1 reference remains `2b32645e26f4ba41de8aa746bd04e84192fb3972`.
Branch remains `model/flowmesh-v2-accounting-test1`; this is one successor
milestone, not an amendment of frozen history.

Review method: primary-author self-review plus fresh Codex contexts for
restoration, reservations/fees, and replay/transfers/V1. This is **internal
review**, not an external Claude or independent security audit. The restoration
context supplied preliminary reproduced observations, not a completed final
report; conclusions were confirmed by the primary or reservation reviewer.

## Findings against the frozen source, before modifications

All following line references are in frozen `test/flowmesh_v2_model/model.py`.

| Finding / code path | Concrete counterexample | Broken requirement | Repair |
| --- | --- | --- | --- |
| Restore checks global totals, not original ownership, 661–674 / 728–734 | Deposit 10 to A; alter only derived spot ownership to B. Restore accepts and B can withdraw. | Original account ownership | Retain bounded synthetic inputs and replay before accepting any restored derived state. |
| OPEN identity but no current lifecycle/history reconciliation, 617–625 / 676–715 | Mark an open reserved order cancelled and release its backing without a CANCEL instruction. | Authorized cancellation and exclusive reservation | Same complete replay check; no fabrication of missing events. |
| Residual recurrence insufficient, 690–707 | Unfilled BUY at 10 funded by 11 accepts spent=10, reserve=0, available=11 while notional/fills remain zero. | Persistent backing and counter consistency | Replay, plus direct revision-spend/fill sanity checks. |
| Revision uses amount instead of sequence ceiling, 690–691 vs 268–269 | Restored revision 4294967296 cannot be named by any permitted CANCEL/REPLACE. | Recoverable valid order identity | Use the action revision ceiling and replay accepted history. |
| Aggregate fees do not prove historical seat ownership, 728–746 | Redistribute 3+2 earned atoms to 0+5 while leaving original trades unchanged. | Historical reward ownership and remainder ordering | Replay exact batch boundaries/fee grouping against frozen configuration. |
| V1 global equation does not preserve initial obligation classes, 750–786 | Change 10 reserved V1 atoms into 10 pending atoms without the original seed input changing. | Segregated preserved obligations | Replay original synthetic V1 seed/settlement/redeposit operations. |
| All-zero REPLACE exits before validation, 345–348 / 270–277 | 17 zero points bypass a limit of 16; repeated/descending prices also cancel successfully. | Declared curve bounds and deterministic refusal | Enforce size/strict-price structure before zero-quantity close. |

Changing derived state while leaving original inputs unchanged is not the
documented coherently-forged-whole-history exclusion. Merely checking summed
custody cannot establish the owner or distinguish pending from reserved funds.

No confirmed additional defect in assigned normal-path replay, transfer or V1
duplicate-credit checks. Original 88 tests passed before repair. The new
counterexample suite ran against the frozen implementation: **10 failures and
one valid-cancellation control pass**. Its complete sanitized failure output is
retained in `review/FAILURES-BEFORE.txt`.

## Minimal model-only correction

No execution economic formula or ordering change. Introduce a versioned
snapshot/2 envelope with bounded canonical inputs for the four existing public
model operations. Rebuild in a temporary model, use normal deterministic
execution, and compare the complete result byte-for-byte. Do not install a
supplied derived state before that comparison. Input ordering/duplicate wrappers
are normalized; exact no-op retries add no history. Record/snapshot limit failures
commit neither money nor outcomes/history. Limits and legacy handling are in
`API.md`; profile/1 itself and its identities are unchanged.

This supplies consistency, **not authenticity**: coherently replacing all inputs
and the matching state still requires an external trusted commitment to detect.
It does not establish real proof validation or rollback/power-loss safety.
Version-1 snapshots lack full risk/batch/seed evidence; their original files and
frozen implementation are retained. No silent upgrade, invented history or
production wallet migration occurs.

Zero-quantity replacement still cancels an eligible owned order. Invalid size or
price ordering now returns `INVALID_CURVE`, consuming only the permitted sequence
and outcome; it does not release the reservation or alter the order counters.

## Independent conservation statement

For every exact AssetId `a`, at a committed model boundary:

```
C[a] = sum(imported V2 deposits[a]) - sum(completed payouts[a])
C[a] = sum(available spot[a]) + sum(owned active reservations[a])
     + sum(backed futures cash[a]) + sum(pending payouts[a])
     + sum(unclaimed FN balances[a]) + treasury balance[a]
```

`C` is backing; the right side allocates ownership/obligations over that backing,
not additional backing. A pending FN/treasury payout has already been removed
from its reward balance and is counted only once as pending. Completed receipts
are history and reduce `C`; they are no longer outstanding liabilities.
`fees_collected` and consumed-funding references are historical counters, not
additional money. The transient candidate fee pool must be zero at commit.

V1 separately retains `V1_custody = available + reserved + pending` and
`V1_initial = V1_custody + unredeposited_external_funding + consumed_funding`.
The consumed term is a source-accounting memorandum linking to one new V2
deposit, not another V2 credit or spendable balance.

The new independent checker derives backing and liabilities directly, and
reconstructs per-account ownership from deposits, accepted explicit transfers,
order lifetime base/notional/fee effects, and pending/completed owner claims.
It calls no Model accounting/invariant helpers. A compensated owner-swap negative
control proves that equal aggregate totals alone cannot satisfy that checker.
It shares model facts/configuration, so this is not a second full implementation
or an authenticated-history proof.

## Qualification protocol

Seeds and counts were chosen before execution: `0xA110`, `0xA111`, `0xA112`,
`0xA113`; exactly 40 batches per replica, two replicas with permuted inputs,
restore both after every 10 batches. Controlled partial-fill/replacement/cancel
cycles coexist with randomized markets, amounts, deposits, settlements,
withdrawals, explicit transfers, six fail-closed risk modes, completed retries,
conflicting content, gaps and new equivocation. No retry-until-pass seed search.

The original 80-batch randomized case and 215-small-curve exhaustive reservation
case are retained. New replay-envelope checks cover count/byte bounds and
atomicity, legacy refusal, original content versus supplied-ID substitution,
integer/boolean distinction, malformed history dispatch and missing evidence.

Newly executed results (Python 3.14.6, standard library):

| Run | Result | Actual exit |
| --- | --- | ---: |
| Original frozen suite | 88 passed, 4.557 s | 0 |
| New regressions against frozen implementation | 10 failed, 1 control passed, 0.021 s | 1 |
| Same regressions after repair | 11 passed, 0.030 s | 0 |
| First repaired original + regression suite | 99 passed, 13.028 s | 0 |
| Replay-envelope focused suite | 9 passed, 0.023 s | 0 |
| Predeclared four-seed campaign + checker control | 5 passed, 50.010 s | 0 |
| Complete suite, Python hash seed 1 | 113 passed, 66.881 s | 0 |
| Complete suite, Python hash seed 2 | 113 passed, 66.945 s | 0 |
| Complete suite, Python hash seed 42 | 113 passed, 66.944 s | 0 |

The three complete runs used the same source and were separate local Python
processes. These are 113 distinct tests repeated, not 339 distinct tests. The
times are suite durations, not trading or network latency. No failing random
seed was omitted or replaced; the retained failing-before regressions are the
only failed suite in this milestone.

Final model source SHA256:
`5890b56818d439cdf738bbc7fd88ee6e7e1a0f284db7803a06b419ef45229caa`.
Unchanged profile SHA256:
`29f94869134b36a936cf974fa4d2ec846f414fe004cfb87e4b530865ba7ae131`.
All executable/test inputs are tracked in this directory plus the preserved
parent; nothing depends on an untracked source snapshot. The exact successor
commit/tree and committed-tree repeat are recorded in the external stage
handoff after committing, avoiding a self-referential commit identifier.

## Economics deliberately unchanged

- Three fresh 9,999-atom orders pay zero; one persistent order filled for 29,997
  pays one atom. There is no new minimum or account-wide accumulator.
- Two 4-atom fee groups sharing exact quote/context in one batch allocate one
  treasury atom; split batches allocate zero because there is no carry.
- Byte-ordered historical seats receive deterministic remainder atoms; no new
  weighting or online-node preference. Fees stay in the exact stable quote.
- Stable fees for non-stable-quoted pairs still need separately approved
  valuation/reservation rules. No conversion rate was invented.

## Scope and next milestone proposal

Only an isolated accounting model is qualified here. No production node/Qt,
wallet, signing key, live funds, custody proof, BFT, PoS, futures margin/oracle/
liquidation, bridge change, migration authority or activation work. Full futures
is still required; explicit cash transfers do not complete it. Ethereum P2P
replacement remains deferred without weakening verification/freshness.
Bootstrap-cutover, lineage-expiry and bridge-identity gates remain open.

Next proposed bounded milestone: **isolated validator-agreement and recovery
modelling**. First identify the exact protocol/variant/profile and approval gaps
(vote meaning and domain, locks/unlocks, commit evidence, view change/restart,
membership/quorum-loss and B3 anchor/cutover rules); then seek approval before
implementation. Do not treat the old V1 patch as V2, pick a variant silently,
reset signing history or reduce quorum to make tests pass. Stop for review here.

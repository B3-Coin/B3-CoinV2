# FlowMesh exact-evidence retry review

Date: 2026-09-10.

Status: **proposal, not an implemented production fix**. The separate honest
lock-split regression described below has been authored in the runtime unit
suite; executable qualification is pending. This document does not identify the
cause of the observed live stall or authorize a protocol change.

## Scope and existing behavior

The proposed change is a local relay-reliability policy only. Keep the existing
wire format, action authentication, auction matching, proposer schedule, quorum,
anchor checks, certificate finality and permanent signing journal unchanged.
The binding decisions are O-9a/O-9b in
[`b3-flowmesh-dex-decisions.md`](b3-flowmesh-dex-decisions.md): no cross-round
unlock, and safe halt on a conflict. Round advancement is not permission to sign
a different hash at the same `(epoch, sequence)`.

Relevant current paths:

- `RuntimeActionPool::Add` and `HandleAction` in
  [`flowmesh_runtime.cpp`](../../src/node/flowmesh_runtime.cpp): authenticate,
  deduplicate and relay an action on first acceptance only. Submitting the same
  action again to a node that already holds it does not rebroadcast it.
- `RuntimeActionPool::EvidenceFor`: the proposal contains semantic action bodies
  with credentials stripped. Before voting, a receiver must match each body to
  separately received authenticated evidence. Missing evidence rejects the
  proposal without voting.
- `MaybePropose`: reuses the permanently locked candidate across rounds. Its
  existing `reannounce_evidence` path broadcasts evidence once after restoring
  a retained candidate on restart, not periodically during an ordinary stall.
- `HandleAttestation`: a frame arriving before a matching candidate is not a
  verified vote and is discarded. Cached-vote replies on later proposals help
  recover lost votes, but are separate from missing action evidence.
- `ProductionSigningGuard::Lock` in
  [`production_engine.cpp`](../../src/flowmesh/production_engine.cpp) and
  `FlowMeshProductionStore::LockCandidate` in
  [`flowmesh_production_store.cpp`](../../src/node/flowmesh_production_store.cpp):
  the durable lock key contains epoch and sequence, not round; a different hash
  returns `CONFLICT`.

Seven sequential actions alone are insufficient evidence of an action-rate
limit. Existing receiver limits are 64 actions/s with a 256-action burst and
256 KiB/s with a 1 MiB burst per peer/market; proposals, votes and certificates
share a distinct 8/s, burst-32 committee bucket. Loss, early arrival, local round
skew, and conflicting signer locks must be distinguished using observations on
the actual signer nodes. An unhalted observer does not prove unhalted signers.

## Smallest proposed retry policy

Reuse the exact `Candidate::evidence` vector already retained before signing.
Do not regenerate credentials, select a fresh action set, rebuild an entry,
change its anchor, or create a new signing position in the retry path.

1. Once a local proposer has successfully retained a candidate, make its
   evidence eligible for a paced sweep beginning on a subsequent tick. A
   restored candidate is immediately eligible for the same bounded scheduler;
   replace its existing unbounded one-shot loop with this path.
2. Only the current eligible local proposer retries: retain all existing
   ready/paused/handoff/anchor/seat-transition checks. No peer message requests
   a sweep or changes its frequency. Signing and proposal delivery remain on
   their existing path and never wait for the sweep to finish.
3. Keep a cursor into the existing evidence vector and a next-sweep deadline
   for the current locked hash. These are memory-only policy fields. Send
   canonical `fmaction` payloads for the original action objects, with the
   original market, epoch and microblock-sequence header.
4. Pace retries across markets with one runtime-wide scheduler. Rotate the
   starting market after each served chunk, inspect at most 16 markets per
   batch, and send at most one bounded chunk per selected market. A fixed map
   prefix must not consume every batch indefinitely.
5. Continue an incomplete sweep from its cursor. After a complete sweep, wait
   at least one second before restarting it. Discard cursor/deadline state on
   certified advancement; never carry it into another sequence. Halted markets
   do not retry. Missing keys or a nonlocal current proposer defer the sweep.

Proposed initial limits, all local policy:

| Resource | Limit |
| --- | --- |
| Scheduler frequency | No more than one batch per 250 ms of the monotonic clock; no accumulation of missed batches |
| Retry frames per batch | 8 across the entire runtime |
| Retry bytes per batch | 32 KiB including each FlowMesh wire header |
| Sustained application-broadcast budget | At most 32 frames/s and 128 KiB/s, with an 8-frame/32-KiB batch burst |
| Scheduler search work | At most 16 market lookups per batch |
| Complete-sweep restart | At least 1 second after the last chunk |
| Additional payload retention | None; index the already retained vector |
| Additional retry state | Constant-size cursor/deadline/hash bookkeeping per locally locked market |

Count and byte budgets both apply; eight maximum-size actions do not fit inside
32 KiB once their headers are included. Repeated `NotifyTick` calls at the same
clock time must not refill a budget. Charge an attempted send even if the
service's reconciliation gate suppresses relay, and retry only at a later paced
opportunity. This avoids a callback-induced spin while preserving eventual
opportunities after reconciliation.

These are application-broadcast budgets, not aggregate wire-egress promises:
existing relay fans a message out to eligible connections. Actual egress scales
with the existing bounded connection count. Initial action gossip also uses
the action lane, so reserving half its sustained budget for retry is not a
guarantee that a busy receiver will admit every retry.

Do not lift existing receiver pool limits. A peer may hold at most 256 admitted
actions per originating peer/market; a maximum 1,024-action candidate cannot
necessarily be reconstructed from a single retransmitting source if the
receiver missed all its evidence. The proposal improves loss recovery for
admissible missing evidence, not arbitrary-state transfer or guaranteed
sub-second certification of maximum-size candidates.

## Required evidence-loss regression

Proposed test name:
`dropped_action_evidence_retries_exact_locked_candidate`.

Use the existing real runtime/store fixtures, deterministic clocks, and a
bounded three-node transport for a four-seat committee. Certify a common
genesis and account funding before leaving exactly three signers online.
Select the next sequence's designated proposer among those three nodes. Use
an actual credential-bearing curve action, signed once by the test account.
Do not use a fake committee, lowered threshold, or fabricated certified state.

Transport schedule and assertions:

1. Deliver the original `fmaction` to the proposer and one voter. Drop every
   copy of that action destined for the third required voter, including gossip
   through another node. Capture the original canonical payload and proposer
   entry bytes. Freeze round changes with a long local test timeout.
2. Allow proposals and votes. Assert the missing-evidence voter has no lock,
   its missing-evidence counter increases, the proposer has exactly two
   verified votes against a threshold of three, and the certified head stays
   unchanged. Run proposal-only retries before enabling evidence delivery to
   demonstrate that proposal repetition alone does not fill the missing pool.
3. Enable delivery of retried action evidence, advance the injected clock to
   the next budgeted opportunity and tick the runtime. Assert retry payloads
   are byte-identical to the captured credential-bearing action. The network
   fixture must not secretly inject the missing action directly.
4. Allow the ordinary subsequent proposal retry. Because proposal priority is
   above action priority, a receiver may reject one proposal before consuming
   the newly queued action; the test must allow that ordering explicitly.
5. Assert exactly one additional certified entry, all three nodes at the same
   head/root, and every persisted lock equal to the original candidate hash.
   Compare entry bytes and hash before/after recovery. A later round may change
   a proposal envelope's round/signature, but must never change its entry.
   If a vote is retried, compare its cached signature bytes too.
6. Tick repeatedly after commitment and inject a delayed old-sequence retry.
   Assert no extra execution, no duplicate account-sequence consumption, and
   no retry cursor surviving into the next microblock.

Add focused pacing variants: repeated ticks at one timestamp; a vector larger
than one chunk; a maximum-size action; two or more markets requiring fair
rotation; restart with retained evidence; reconciliation suppression followed
by recovery; and halt/epoch/sequence changes. Assert emitted count and framed
bytes, not wall-clock throughput. Existing wire admission limits remain in
force in every variant.

An early-attestation cache is explicitly a separate proposal. It would require
global/per-peer/per-position count and byte bounds, TTL and deduplication, with
signature verification only after a candidate is fully validated. Cached
unverified frames must never contribute to `max_verified_attestations`.

## Honest divergent-candidate regression

Round skew alone does not imply different locks. If every voter sees the same
candidate, the current adjacent-round rule can reunite timers; the existing
`authenticated_future_round_reunites_split_validators` test exercises this
using deterministic empty genesis. That test does not cover different valid
action sets or anchors at an ordinary sequence.

The new test
`honest_divergent_round_candidates_preserve_permanent_locks_and_halt` in
[`flowmesh_runtime_tests.cpp`](../../src/test/flowmesh_runtime_tests.cpp)
implements this deterministic scenario without modifying runtime code:

1. Four honest seats A/B/C/D certify the same genesis. All share the same
   canonical mature anchor, seat set, deposit facts and certified state.
2. Temporarily delay traffic between A/B and C/D. Deliver one valid deposit
   action X to A/B and a different valid deposit action Y to C/D.
3. At sequence 1, round 0, the designated proposer B proposes X. A and B lock
   X. B collects exactly two verified attestations, below the threshold of
   three; nobody advances the certified head.
4. Advance the C/D clocks, then tick C. At sequence 1, round 1, C is the
   designated proposer. C and D execute and lock Y. C also collects exactly
   two verified attestations. Durable lock hashes are X/X/Y/Y, with X != Y.
   All four nodes are initially unhalted. No seat has signed two candidates.
5. Restore transport and deliver each missing action. Both action sets can
   now be independently executed everywhere, but existing locks remain.
6. Retry C's authentic round-1 proposal. A/B are in round 0, so this proposal
   is round-admissible. Re-execution succeeds, but retaining Y conflicts with
   their permanent X locks: A/B safely halt. All four durable locks and the
   common certified head remain unchanged.

This is an executable characterization of the accepted safe-halt boundary,
not a test that should demand successful certification after conflicting
locks have already formed. A useful additional variant holds actions identical
but lets the two proposers choose different still-canonical mature anchors;
it must first prove both candidate anchors pass existing checks.

## Why retransmission cannot repair this split

With four seats, each candidate has two locked signers but needs three.
Replaying exact evidence changes neither candidate identity nor the durable
locks, so it cannot create the third compatible signature for either candidate.
An action retransmission fix may prevent some missing-evidence stalls before
such a split develops; it is not a view-change protocol.

Do not delete journals, reset a signing position, reinterpret a lock as
round-local, reduce the quorum, replace the candidate's anchor, or import an
unverified peer tip to move past the conflict. Any general liveness design for
this boundary needs a separately reviewed protocol-level proposal consistent
with the owner's permanent-lock decision. Timer tuning and evidence retries
can reduce opportunities for divergence but do not prove that divergence is
impossible.

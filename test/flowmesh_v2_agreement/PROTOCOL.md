# Milestone 2: exact isolated agreement test profile

Status: test-model specification, not approved production consensus. Written
before implementing transitions. Frozen accounting is unchanged at
`9d4fd53dc7d58d397680fd64065663703cbbdd17`; preserved R1 is
`2b32645e26f4ba41de8aa746bd04e84192fb3972`.

Profile: [`TEST_PROFILE.json`](TEST_PROFILE.json),
`flowmesh-v2-single-sequence-pbft-test/3`. The original `/1` protocol and
counterexamples remain in frozen history at
`0247001a04588fccbadf63bb9d4a8704dcaf5b2c`. The
[Milestone 2.1 delivery supplement](MILESTONE_2_1.md) specifies `/2`'s
non-voting admission/retry changes; the voting predicates below are unchanged.
The [Milestone 3A repair supplement](../flowmesh_v2_storage/REPAIR_R1_R2_R3.md)
records `/3`'s bounded received-vote policy, atomic view-entry/report intent,
and enforcement of the already specified timer predicate. Frozen `/2`
and its failing-before evidence remain at `1d022dbf0da63636d6d43650d3864222761497a1`.
Reference: Castro/Liskov, [PBFT, OSDI 1999 §4.2–4.5](https://www.usenix.org/legacy/publications/library/proceedings/osdi99/full_papers/castro/castro_html/node4.html).
R1 mapping: [C1–C6](../../doc/design/v2-stage0-r1-consensus.md).
The shared concepts are authenticated proposal, preparation, commitment, and
highest-prepared selection using a quorum of view-change reports. This is not
HotStuff or a mixture of its lock/commit rules.

## Authority and failure assumptions

Two configured equal-weight rosters only: N=4/f=1/q=3 and N=7/f=2/q=5.
Indexes are fixed configuration, never peer arrival or online status. The
common initial accounting snapshot, genesis parent identifier, epoch zero and
synthetic finalized anchor are explicit trusted test inputs. Their authority
does not come from a vote in this model. No committee change, mainnet bootstrap,
weighted PoS or bridge threshold is implemented.

Safety assumes no more than f Byzantine identities, unforgeable authenticated
messages, collision-resistant test hashes, exclusive signer ownership and
stable acknowledged records. Additional honest nodes may crash/be unavailable
or partitions may prevent progress; no threshold changes. Progress requires a
responsive compatible quorum, eventual timely delivery and payload availability,
an honest proposer turn and timeouts long enough within the finite test bounds.
Malicious members can sign arbitrary messages as themselves but cannot use an
honest signing capability. This is a simulator trust boundary, not cryptography.

Crashes discard volatile state, not acknowledged durable state. Intent and
signature persistence precede publication. Known or suspected restored-old
state permanently fences local signing in this model, with read/catch-up still
possible. No model operation clears that fence. A coherent undetectable old
backup cannot be detected by ordinary integrity checks and is outside the
guarantee. Stable-memory injection is not filesystem/power-loss qualification.

## Identities and payloads

I = (profile, domain, configuration hash, epoch, ordered-set hash, decision
sequence, parent ValueId). Each next I is derived only after the preceding
committed batch is applied. Application initialization/configuration and the
fixed roster are committed by configuration identity. Parent identity is the
value, not a particular proof subset.

Body = (I, exact anchor height/hash/context, canonical batch, resulting
accounting-state commitment). ValueId hashes the versioned body; view and phase
are excluded. Canonical batch normalizes exact repeated facts/actions and sorts
by canonical bytes; the unchanged accounting model defines economic ordering.
Accounting preview clones state and executes without mutating the live ledger.
Receivers reexecute and check the commitment. Re-proposal keeps exactly the same
body and actions. Anchors 855500 and 855501 produce different values for the same
I/actions; consecutive sequences instead have different decision identities.

Signed messages bind versioned phase, signer index, I, view, ValueId and complete
phase-specific payload. Phases: PROPOSE, PREPARE, COMMIT, VIEW_CHANGE, NEW_VIEW.
Synthetic authentication verifies the exact minted message, not a boolean from
the sender. Proposal bodies are delivered/fetched separately; missing data gives
NEED_DATA with no vote/application. Proofs carry signed headers and nested
justifications; lists are canonically ordered distinct signers, q through N.

PreparedQC = authenticated scheduled PROPOSE + q matching PREPARE signatures.
CommitQC = PreparedQC + q matching COMMIT signatures. A nonzero-view PROPOSE
contains the exact valid NEW_VIEW justification. NEW_VIEW contains q signed
VIEW_CHANGE reports and the selected ValueId. Each report contains its highest
durable PreparedQC from a strictly older view and any known CommitQC. All
proofs and reports use exact I. No private latest B3 tip enters validation.

## Durable / volatile state

Per replica durable: configuration/genesis, current sequence and view/mode,
accepted proposal per view, accepted NEW_VIEW per view, highest prepared proof,
exact intent and signed message per (I, phase, view), stored decisions and
application markers/snapshots, exact retained outgoing messages, and freshness
fence. No pruning/reset occurs to make progress. Per replica volatile: inbox,
body/evidence cache, individual received votes/reports, validated proof cache,
timer/deadline, pending data/retry work and local diagnostic tip.

## Transition table (normative for this test profile)

| Event | Guard / evidence | Durable change before outgoing vote or success |
| --- | --- | --- |
| Start/restart | Exact configured initial state or preserved durable state; exclusive synthetic owner | Restore all safety records and application markers; finish pending committed application exactly once; no permission from a fresh process alone |
| Offer local body | I is current; deterministic execution/anchor evidence valid | Retain exact offered body (not an executed ledger change), start pending-work timer, disseminate bounded OFFER to configured replicas |
| PROPOSE at view 0 | Correct scheduled signer `(sequence+view)%N`; current ACTIVE view; body valid; no conflicting accepted proposal | Persist exact accepted header/body before PREPARE intent |
| PROPOSE at view >0 | Same, plus matching validated durably accepted NEW_VIEW | Same; no naked vote can manufacture proposal/NV acceptance |
| PREPARE | ACTIVE/current; accepted valid body and exact NV if nonzero; fresh signing state; unused or identical phase/view slot | Persist intent, generate signature, persist exact signature, then publish; identical retransmission only |
| Learn PreparedQC | Complete valid q-proof; same I; body/evidence available before local use | Keep highest prepared view; never downgrade on arrival order. Conflicting equal-view complete proofs halt. Retain older valid evidence separately |
| COMMIT | ACTIVE/current; matching durable accepted proposal, PreparedQC and NV; fresh signing state | Persist proof and exact intent/signature before publication |
| Local timeout | Pending current work; real timer expiration; ACTIVE or CHANGING with q current-target reports | Persist view+1 CHANGING, abandon old-view vote creation, publish one immutable VIEW_CHANGE report; no unlock or roster change |
| View synchronization | f+1 distinct valid reports for the same higher bounded target, or a fully valid NEW_VIEW | Advance monotonically to CHANGING and issue truthful report as applicable; a single arbitrary higher integer has no effect |
| Build NEW_VIEW | Scheduled proposer; q valid reports for exact target; all preparations from older views | If any contains decision, learn that decision instead. Otherwise select greatest prepared view, or any locally valid offered value if none; persist one exact NEW_VIEW; preserve omitted local preparation |
| Accept NEW_VIEW | Scheduled signature, exact I, q reports, verified highest selection, valid body; target >= current | Persist NV and ACTIVE path for its exact value, then accept matching PROPOSE; do not require omitted local QC in report set; never replace a decision |
| Old proposal/vote | Authenticated but abandoned view | May complete evidence; no fresh old-view vote, backwards view movement or timer reset |
| Complete CommitQC | Valid full proof from any view; valid available body; exact current or recorded I | Record immutable decision; apply cloned preview and application marker atomically/replay-safely; only then client-visible completion and next sequence |
| Missing body/anchor/parent | Valid relevant reference but local evidence unavailable | Bounded body/anchor defer and exact request; unknown future-instance messages are not retained, and trigger bounded discovery of the local next certificate. No vote/guess/rewritten candidate |
| Duplicate final proof | Same decided ValueId (any valid signer subset) | No second execution, economic effect, or next-sequence allocation |
| Conflicting valid decision | Same I, different ValueId | SAFETY_HALT with evidence; never reverse finalized value |
| Storage/freshness/resource failure | Injection, known rollback, indispensable safety/audit bound | Explicit signing unavailable/halt; preserve all acknowledged obligations. Disposable network/cache pressure instead refuses/evicts/retries under the supplement; it does not reset safety state |

Late learned preparation is retained for future reports, never used to rewrite
an already signed report for the same tuple. A report may only carry preparation
from below its target. A future-view proof is not a naked permission to vote:
its matching NEW_VIEW must first be accepted, or the node defers it. A valid
older proof remains available even if the current path supersedes it.

## Pacemaker and data availability

Test time uses integer ticks, not milliseconds. Initial active pending-work
deadline is 30 ticks; later view timeout doubles up to 240. CHANGING waits for q
same-target reports before starting its next-view timer. f+1 same-target reports
can synchronize laggards. Invalid/duplicate traffic does not postpone deadlines.
Retries every 10 ticks retransmit exact retained objects, request missing bodies
and advertise final proofs. Timeouts never choose an economically different
object by themselves. No extra user trade is needed to finish the last batch.

Recovery clarification after the retained counterexamples: a restart restores
current-instance offered bodies and accepted/prepared headers from its own
durable records. Active pending work regains a timer; a CHANGING node still
waits for the specified report quorum. A decided-but-unapplied node missing
anchor evidence requests that exact evidence and resumes the same decision;
it neither signs replacement work nor mistakes that decision for application.

View entry and the exact VIEW_CHANGE report intent now commit atomically.
The report carries the locally retained highest preparation at that transition;
the following ordinary intent/signature persistence and exact publication
path is unchanged. An interrupted transition either leaves the old view or
the new view with its report intent, never a new view without that obligation.
All timer callers use ACTIVE, or CHANGING with q validated same-target reports;
request data, queued work and restart cannot stand in for those reports.

An unfinished candidate-signing intent likewise revalidates its exact body
against locally available anchor evidence before its **first** signature.
Missing evidence is requested on restart, on received data, and by the bounded
retry timer. The intent is retained unchanged; evidence arrival does not permit
signing an abandoned view. Already-issued signatures may be retransmitted
unchanged without generating a new signature. The historical checker records
the local body/evidence at first signing; later/global evidence cannot repair
an invalid past guard. This implements the existing missing-data rule, not a
new phase, timeout or quorum rule.

OFFER is synthetic client ingress, not a vote or new consensus phase. Initial
ingress and bounded retries disseminate its identical body, including from a
nonleader. Each receiver validates the body before retaining pending work.
Receiving it does not execute the ledger or grant any signing exception.
There is no new production networking code or claim of authenticated client
transport here. Current-instance retention is bounded; stale bodies cannot
be rebased into a new economic instruction.

An accepted NEW_VIEW is exact: a different signed report bundle for the same
view is rejected even when its selected ValueId is equal. A proposal cannot
substitute that bundle for the one durably accepted. This does not change
ValueId or prevent a later valid view from carrying the same value.

Synthetic anchor evidence is a configured linear finalized chain extending the
agreed parent anchor; each node has its own evidence store. It verifies exact
height/hash/context, monotonic descendant relationship, and test import facts.
Local tip observations are diagnostics only. There is no live B3 verifier,
registry, fork-choice, event-completeness or authenticated custody qualification.

## Explicit departures / exclusions

- Single outstanding sequence instead of PBFT's pipelined window and stable
  checkpoint garbage collection. All test safety history is retained, bounded.
- q PREPARE votes include the primary, rather than the reference's primary
  pre-prepare plus 2f backup prepares. COMMIT still needs q; no lower threshold.
- Proposer includes the sequence offset specified by R1. Highest proof selection
  is single-sequence; if none, a valid offered batch is permitted rather than a
  gap-filling null request. No-op accounting batches are valid explicit values.
- R1's same-target f+1 synchronization (not any mixed higher-view reports).
- Complete transferable synthetic proof bundles; JSON/SHA256 test codec and
  synthetic unforgeable capabilities instead of production BLS/MAC codecs.
- Explicit bounded simulator durability, fencing, retries and data requests;
  not production storage/network/client code or a replacement FlowMesh network.

Every bound is declared in the profile. Reaching maximum view/sequence or a
storage/work limit stops signing; no liveness claim beyond those limits. Tests
must report actual explored schedules/faults separately from fair-network
progress. A finite exploration is not a theorem over every schedule.

The checker inspects all generated signatures, even withheld/unpublished ones,
and counts potential quorums independently, not just delivered certificates.
Bounded test-only events also record exact intent, historical signing evidence,
signature persistence and every publication (including retries). The checker
validates their order and objects, not merely eventual durable state. Its
anchor-ancestry walk is independent of replica acceptance; the simulator's
initial synthetic evidence is retained for the audit, never used as a global
candidate-selection oracle by replicas.
Hidden COMMIT q implies at least f+1 honest prepared holders; their later view
reports intersect every q report set. This motivates, but does not replace,
the adversarial tests. A lone omitted prepared holder cannot veto a valid
NEW_VIEW. Nodes never inspect other nodes' private state to decide anything.

Unresolved later gates: bootstrap/cutover, lineage expiry, fixed-to-changing
membership, genuine PoS V2, bridge AssetId/RegistryId and unchanged verifier
compatibility, stable-fund authority, real futures risk and production codecs,
storage, fencing and WAN performance. V1 split locks/signatures remain intact.

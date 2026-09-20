# Stage 0 R1 — proposed commitment, authority and recovery rules

Status: **PROPOSED, NOT RATIFIED, IMPLEMENTED OR QUALIFIED**. This companion
supplies deterministic rules missing from §§7–8/13 of frozen Stage 0
`f327bd2cc334282eec17a09c1d15d04f68b7f4c5`. It does not change that revision.
It specifies a candidate for review, not a safety proof or permission to
activate it. Every new rule below requires D1/D3/D5/D10 approval as applicable.
No production height, deployed authority or economic parameter is selected.

The candidate remains **single-sequence PBFT-style prepared/commit**. It is
not HotStuff, and no HotStuff lock/pacemaker rule is imported by implication.
The initial implementation must model the rules below independently before
using V1 components. This document distinguishes deterministic public
validity from a particular signer's private safety/readiness obligations.

## C1. Instance, value and proof identities

An immutable instance context is:

```
I = (networkDomain, protocolVersion, subsystem, deploymentId,
     authorityEpoch, committeeHash, sequence, parentValueId, configHash)
```

`subsystem` distinguishes FlowMesh execution, B3 checkpoint agreement and
the one-time transition agreement. `sequence` is a monotonically increasing
decision ordinal, not a height, view, tip or packet counter. `parentValueId`
identifies the canonical decided **body**, not any particular CommitQC's
bytes: two valid signer subsets proving the same body do not create two
parents. The next context and committee come from the parent state or the
authorized bootstrap in C8, never from a candidate's self-declared roster.

`ValueId = H(versioned value tag || I || canonical candidate body)`.
The body commits ordered application actions/evidence, anchor transitions,
result/effect roots and membership transitions, if any. Consensus view and
proof encodings are outside the body. A value selected by NEW_VIEW can
therefore be re-proposed unchanged in a later view. Its execution parent,
anchor and effects cannot be silently refreshed at that time.

Signatures bind `I`, phase, view, ValueId where present, signer index and
the complete phase-specific payload. Distinct versioned tags are required
for PROPOSE, PREPARE, COMMIT, VIEW_CHANGE and NEW_VIEW. These are **not** the
existing `B3/FINALITY/V1` Ethereum-export domain. Exact codecs, integer widths,
proof-size limits, approved cryptographic suite and byte vectors remain G1
freeze gates; deployments with different configurations have different I.

Committee indexes are the canonical ordered indexes committed in
`committeeHash`; message arrival order cannot renumber them. For FN seats,
every certificate/report quorum has at least `floor(2N/3)+1` distinct seats.
For B3 finality and transition authority, that headcount **and** the existing
`floor(2W/3)+1` weight threshold are required for every quorum phase and
NEW_VIEW report set, not just the exported legacy certificate. Duplicate
identities never add weight. Proof lists are sorted by index and reject
duplicate or unknown signers. A proof can contain more than the minimum
threshold; its subset is not part of ValueId.

Safety assumes Byzantine headcount below one third, and, for weighted
authority, Byzantine weight below one third. Progress additionally requires
an available qualifying quorum, eventual timely delivery, valid available
application data and continuing honest proposer turns. Those assumptions
do not hold merely because connected peer count exceeds a threshold.

## C2. Pure public application validity

Define `ValidValue(I, parentState, body, suppliedEvidence)` as a pure function.
Its result is `VALID`, `INVALID(reason)` or `NEED_DATA(exact identities)`.
The last result does not authorize signing. It does not read local latest
tip, mempool arrival order, wall-clock timeout, peer count or wallet history.
Every body/root/evidence bound is checked before resource-intensive work.

Common requirements: exact I/parent/config; no overflow; canonical body and
ordering; every referenced object available and authenticated; deterministic
execution produces the supplied roots; authority changes obey C7/C8.
An invalid input does not become valid because several peers repeat it.

### FlowMesh value

Verify the full committed parent state, ordered action authorization and
shared account sequences, exact-AssetId accounting, reservation/custody
rules, oracle/risk configuration where enabled, and C6 anchor/import rules.
Re-execute on scratch state. All externally originated facts are carried
as bounded evidence against the selected finalized anchor; execution does
not query an endpoint for a fresh fact halfway through a batch. Futures
actions with an undefined risk rule fail closed under the accounting
proposal; this is not a substitute for the release-required futures design.

### B3 checkpoint value

The parent B3 decision supplies the current checkpoint authority
`(e, Set_e, Set_(e+1), epochFloor)` and the last committed checkpoint F.
The candidate contains scheduled checkpoint `(h, hash)`, authenticated
validated branch data from F through a witness block W, cumulative bridge
withdrawal root at h, the exact committed successor header, and any C7
terminal-epoch transition. Public validity requires:

1. `h > F.height` and the inherited checkpoint grid
   `(h - modernStart) mod checkpointInterval == 0`. Skipping eligible heights
   is permitted; no independent agreement instances exist for skipped
   heights. The current sequence still decides exactly one value.
2. The branch descends from F and validates every intervening block under
   its historical/versioned rules. W is on that branch and
   `W.height >= h + checkpointDepth`. The configured inherited consensus
   depth is 12 on the inspected mainnet baseline. The witness proves depth
   and branch validity, not finality of W. Only h is the checkpoint pin.
3. Root, signing epoch and successor header equal the values derived by
   C7 and validated branch state; no candidate-supplied substitute authority.
   The cumulative withdrawal tree and all old nullifiers remain continuous.
4. The supplied parent decision/carrier evidence required by C7 is present.
   Referenced old certificates satisfy their original V1 validation rules.
   The witness suffix must validate without this candidate's own CommitQC,
   its own legacy certificate, or a decision descending from it. Only already
   independently authenticated predecessor carriers may supply its authority.
   Reject a witness that requires the proof being constructed to validate
   one of its blocks; do not resolve that circularity by provisional trust.
5. Public bridge-format/set constraints in the approved config are met.
   Actual Ethereum acceptance additionally depends on its separately
   verified current lineage and wall clock; C9 never claims otherwise.

The candidate body's commitment to a depth witness does not finalize its
suffix above h. If that suffix becomes a stale local fork, nodes can still
validate the historical witness. They cannot change the voted body's
checkpoint, roots or witness bytes to fit their preferred tip.

**Depth distinction:** V1 certificate inclusion uses 12-block depth; the
separate mainnet *local production policy* waits 20 blocks before creating a
new finality signature. Neither is proof that a node must already prefer
the proposal's branch. The R1 proposal retains the extra 20-block wait as
proposer/export readiness policy, satisfied by authenticated descendants
of h supplied to that node, not as a received-candidate validity check.
Receipt of a fully valid 12-deep proposal can be PREPARE/COMMIT eligible.
Legacy export can remain `WAITING_FOR_SIGNING_DEPTH` until the extra policy
is satisfied. Changing that policy would be a separate explicit choice.

`MaySignLocal` is separate from ValidValue: key membership, exclusive signer
ownership, durable-state freshness and retained V1 obligations must pass.
An orphan-locked signer reports `LOCAL_LEGACY_OBLIGATION_BLOCKED`; it does
not label an otherwise publicly valid branch invalid or discard the lock.
Before PREPARE, bridge-compatible B3 signers must establish ancestry and
lineage compatibility of the proposed final external object with retained
history. A missing quorum of such signers blocks progress honestly.

## C3. Durable replica state and normal transitions

For each I retain:

```
currentView, mode = ACTIVE | CHANGING | DECIDED | SAFETY_HALT
acceptedProposal[view], acceptedNewView[view]
highestPreparedQC
localIntent[(phase, view)], exactLocalSignature[(phase, view)]
decision(body, CommitQC), appliedMarker, nextContext
```

`PreparedQC` contains a qualifying PREPARE quorum for one I/view/ValueId.
`CommitQC` contains a matching PreparedQC, qualifying COMMIT quorum and,
for nonzero view, matching validated NEW_VIEW justification. Each vote
binds the same I/view/value. A signature by one replica is not a lock
release, commitment or client certification.

Proposed scheduled proposer:
`proposer(I, view) = (sequence mod N + view mod N) mod N`, using the committed
ordered roster and checked arithmetic. This is unweighted leader rotation;
B3 vote weight remains weighted. It changes checkpoint coordination, not
the BIP340/PoS block-production eligibility calculation. No persistent
leader optimization is selected.

| Event | Required guard | Durable transition before any outgoing signature |
|---|---|---|
| Start I | Verified parent/authority; C5 freshness; no existing different context at sequence | Install I and view 0 ACTIVE, or resume its existing records |
| Accept PROPOSE at view 0 | view equals currentView; correct proposer signature; ValidValue; no decision/conflicting accepted proposal | Record body/proposal acceptance |
| Accept PROPOSE at view >0 | Above guards plus complete matching NEW_VIEW accepted durably for this same view/value | Record body and proposal; leave CHANGING only through that NEW_VIEW |
| PREPARE | ACTIVE; accepted proposal at currentView; ValidValue; MaySignLocal; matching accepted NEW_VIEW if view>0; no different PREPARE intent/signature at tuple | Persist exact intent, sign only that intent, persist bytes, then publish |
| Learn PreparedQC | Valid quorum/context/body linkage | Retain complete highest-view QC before using it in COMMIT or a view report |
| COMMIT | ACTIVE; view equals currentView; matching durable accepted proposal and PreparedQC; matching NEW_VIEW if needed; MaySignLocal; no conflicting COMMIT intent/signature | Persist QC then exact COMMIT intent/signature before publication |
| Learn complete CommitQC | Full proof and body valid, even from an old view; no different decision | Durably record decision and atomic/replay-safe state/effects/application marker |
| Advance sequence | Decided value durably applied; exact next context derived from its state | Install next I, preserving predecessor proof/history |

No receive handler may manufacture a missing durable proposal/NEW_VIEW
guard from a naked vote. Late PREPARE/COMMIT may complete a proof, but cannot
cause a new vote in an abandoned view. Application/client final success
requires the durable decision/application boundary, not PREPARE quorum,
COMMIT broadcast or a socket acknowledgement. A local storage failure
prevents publication and makes signing unavailable.

## C4. View change, safe selection and timeout semantics

Local progress timeout may request exactly `currentView+1`. Persist that
monotonic view and CHANGING before publishing VIEW_CHANGE; stop creating old
view votes. A valid `f+1` distinct authenticated reports for the same higher
target can cause a lagging node to enter that target, with
`f=floor((N-1)/3)`, provided the target is in the bounded validated instance.
This is only a pacemaker trigger. It is not a quorum or proof allowing votes.
An arbitrary peer integer or timeout never authorizes a new candidate.

Every VIEW_CHANGE binds I/target/signer and includes the highest complete
PreparedQC known durably at signing, plus any known decision. A local
COMMIT record necessarily retained its PreparedQC. A replica must not
conceal that proof in its report. A later learned higher PreparedQC is
retained and sent as evidence, and included in the next view report; the
already-signed report is never rewritten under the same tuple.

For a target view, only its scheduled proposer can sign NEW_VIEW. It
includes a qualifying quorum of distinct signed reports and the selected
body. All report contexts match I; each attached preparation is from a
strictly earlier view. Select the value with the greatest prepared view in
those reports. If none includes preparation, select any ValidValue. If
equal greatest views certify different values, enter SAFETY_HALT with
evidence. A report containing a valid decision requires applying that
decision instead of initiating further voting.

The follower verifies the complete reports, quorum, preparation proofs,
highest-proof selection, correct proposer and candidate data. Acceptance
persists NEW_VIEW and its proposal before ACTIVE/nonzero-view PREPARE.
For an already higher durable currentView, the proof may be retained but
does not move the node backwards or permit fresh old-view votes. A complete
valid CommitQC from any view is still applied under the decision rule.

With no local decision, a valid NEW_VIEW may supersede a local PreparedQC
or individual COMMIT even if that proof was absent from the chosen report
quorum. There is **no omitted-local-QC veto**. Keep the omitted proof and
report it in subsequent view changes. This relies on honest quorum
intersection and honest COMMIT signers carrying their prepared proof in
future reports; it requires adversarial model checking, not an assertion
that the PBFT name supplies a proof. A conflicting fully valid decision
can never be superseded; if conflicting proof families verify, halt.

Timeout durations/backoff ceilings are unselected operational parameters.
Timeouts affect progress only, never signed meaning, safe voting or set
membership. Authenticated duplicates/invalid traffic do not reset progress
timers. Views have a checked, nonwrapping width; exhaustion is explicit
unavailability, not journal deletion. Bounded recovery queues retain exact
critical proof identities/bytes and request missing data. Bulk backpressure
cannot downgrade these proof requirements or create new votes.

## C5. Restart, durability and rollback boundary

On ordinary crash/restart, replay the durable records before enabling a
key. Verify context/authority, monotonically increasing sequence/view,
intent/signature equality, required proposals/QCs, decision/application
markers and anchor/import/nullifier continuity. An unfinished unsigned
intent may finish only its exact object while its recorded view is still
authorized. Signed bytes may be retransmitted exactly; no recreated action,
replacement sequence or competing vote is a recovery shortcut.

The proposed crash fault model requires acknowledged journal writes to
survive ordinary crashes/power loss and an exclusive fenced key owner.
Filesystem/process locking alone does not establish exclusivity across two
machines. Signing deployment must establish that the previous owner cannot
continue, including during failover. The transport public key is not this
fence and an RPC `armed` flag is not evidence of it.

**Coherent rollback is not detectable from journal integrity alone.** A
valid old backup can omit already-published signatures. This proposal does
not claim software can detect every such restoration without a separate
nonrollbackable witness. Known/suspected snapshot restore, lost safety
records or ownership uncertainty enters `SIGNER_FRESHNESS_UNPROVEN`; reads
and proof catch-up can continue but signing cannot. Peer agreement on a
head does not prove that the signer never issued an omitted vote.

For the initial bounded consensus model, coherent undetectable rollback is
explicitly outside its crash assumption; detected restore must fail closed.
Production restart/transfer qualification additionally needs approval and
evidence for an anti-rollback deployment profile (for example monotonic
hardware/fenced signing service), or an explicitly accepted threat boundary.
No such infrastructure or trust provider is chosen here. Known old backups
are never automatically promoted to fresh signing state. A model pass under
stable storage is not anti-rollback qualification.

## C6. FlowMesh finalized anchors and import cursors

The parent FlowMesh state stores `(anchorHeight, anchorHash, finalityProofId)`
and one canonical B3 event import cursor. A proposal either retains the
exact anchor or carries a strictly higher descendant with a complete
authenticated B3 finality chain under C7/C8 and original V1 rules where
applicable. An equal-height different hash, lower height, missing authority
transition or conflicting final proof is rejected; a genuinely conflicting
valid final proof is a safety incident. No highest-tip or fastest-peer rule.

A cursor is `(blockHeight, transactionIndex, eventIndex)` in the authenticated
B3 branch. Imported external events are the next bounded contiguous prefix
in canonical block/transaction/event order after the committed cursor,
never an arbitrary skipped credit chosen by a proposer. Cursor advancement
includes verified absence of relevant events in skipped blocks. The complete
block data or an approved equivalent completeness proof must establish that
prefix. The size bound is signed config. A proposal may import zero events;
internal trades can proceed using already recognized custody.

Raising the anchor does not itself credit everything below it. The import
cursor may lag the anchor, but never exceed it. Each deposit outpoint and
settlement receipt has its original namespace/nullifier; duplicate evidence
is recognized without a second economic effect. Pending settlements remain
separate reconciliation liabilities until their exact import. Snapshot and
catch-up proofs preserve the cursor, pending buckets and all consumed sets.

Version routing at symbolic H is defined by the
[transition companion](v2-stage0-r1-transition.md). A pre-H V1 deposit remains
V1 even if discovered later; a V2 cursor cannot claim it merely because the
asset is supported in V2. Reorg above the committed anchor removes only
uncommitted observations. It cannot rewind the cursor, revive a nullifier,
re-sign a user request or reverse a certified internal trade.

## C7. Authority transitions and the proposed PoS separation

### FlowMesh authority

The first FN roster comes from C8. Later terminal decisions by the current
FN committee bind exact successor roster/config, terminal state/value and
the B3 finalized registry anchor used to derive that roster. The candidate
uses the existing anchored seat eligibility and canonical ordering, not an
online list. The parent context fixes the next terminal sequence/epoch
condition through an approved configuration; a candidate cannot select its
own epoch length. Exact cadence is a genuine unselected parameter and
therefore the membership-transition model must use named synthetic values
until approved.

A terminal decision does not itself transfer a V1 market's custody. After
it is durable, sequence+1 uses the successor committee, extends the terminal
ValueId and retains all ledger state. Old votes cannot decide that new
context. No terminal quorum means no transition: automatic removal/weight
decay or emergency lower threshold is not part of this profile.

### B3 checkpoint authority: P3-R1 proposal, requiring D3/D5 approval

**Separate checkpoint authority from the producer snapshot tracker.** V1
currently uses inclusion-triggered epoch rotation and derives a new successor
at the actual rotation boundary minus one. Two branches carrying the same
certificate at different heights can therefore derive different subsequent
snapshots. Sharing that tracker unchanged with a single global checkpoint
decision ordinal cannot simultaneously guarantee unique next-instance
authority. This is the precise boundary being changed, not an already
approved architecture.

Proposed checkpoint state is `(e, Set_e, precommitted Set_(e+1), epochFloor,
lastDecision)`. At bootstrap preserve the current and already-committed next
headers exactly. Ordinary decisions within e keep both fixed. The first
committed checkpoint h with `h >= epochFloor + E`, where E is the inherited
finality epoch block interval, is terminal for e. The terminal flag is
derived, not optional. Its internally committed body also fixes:

- `nextEpochFloor = h+1`;
- incoming authority **exactly** the already-precommitted `Set_(e+1)`;
- `Set_(e+2) = BuildV1Snapshot(epoch=e+2, validated branch state at h)`, using
  inherited eligibility, weights, canonical ordering and carry-over rules
  if the inherited minimum-set floor is not reached.

This deliberately changes the **checkpoint** snapshot boundary from actual
carrier inclusion−1 to committed checkpoint h. `Set_(e+2)` is not a new
authority for the terminal vote; the outgoing `Set_e` authenticates it.
The terminal external V1 attestation still commits only the fixed
`Set_(e+1)`; the first incoming-epoch export commits `Set_(e+2)`.

Next ordinal uses incoming e+1 and must name a higher scheduled checkpoint
on a branch containing the prior terminal proof carrier below that new
checkpoint. The carrier is a later block containing the prior CommitQC and
its compatible legacy certificate; it cannot be the checkpoint whose hash
that QC signs. Missing legacy export therefore can stop next-epoch finality
without replacing the committed terminal value. Inclusion at different
heights cannot alter the already-authenticated checkpoint roster/snapshot.

The proposal retains V1 producer eligibility arithmetic, stake ownership,
snapshot construction, reward economics and its handover-gated rotation
timing pattern. Its **producer** snapshot is derived from the producer
branch at that timing boundary as before; it is not reused as V2 checkpoint
authority or automatically exported as an Ethereum successor. Historical
blocks still use their original shared tracker. To make the proposed post-H
handover signal deterministic, producer state stores its own `producerStart`,
current/next producer snapshots and `handoverSeen`. A valid V2 finality
carrier sets `handoverSeen` only if its finalized checkpoint height is at
least `producerStart`; producer epoch numbers are not compared with the
now-separate checkpoint epoch. At the first subsequent block height
`b >= producerStart + E` with `handoverSeen` already true below b, rotate
to the precommitted next producer snapshot, set start to b, clear the flag
and derive the following producer snapshot at b−1 using the inherited
snapshot/carry-over rule. A reused old carrier whose checkpoint is below
the new start cannot authorize another rotation. If no carrier qualifies,
retain the current producer snapshot; no private timeout substitutes for it.

This preserves the **handover-gated timing pattern**, not byte-for-byte
identity of V1's shared-epoch predicate: replacing `certificate.epoch ==
producerEpoch` with the explicit height attribution above is part of the
unapproved D3 change. Post-H code must separate the two views and enumerate
each call site. Independent block-validation vectors must show inherited
producer eligibility arithmetic/timing when the views align, and the exact
deliberate authority difference when carrier heights or epochs diverge.
This is a source/refactoring and hard-fork compatibility gate, not a claim
that existing code already supports two trackers.

Base block production may continue on its last authorized producer state
while checkpoint agreement/export is blocked, subject to all retained
production validity rules. It does not promote an uncommitted checkpoint
roster. No alternative that silently freezes all production at an arbitrary
handover boundary is selected. If the owner instead wants producer and
checkpoint authority always identical, that is a different D3 choice and
requires a new deterministic boundary design, including its no-quorum stop
behavior; both designs must not be mixed.

New checkpoint authority is not permission to defeat Ethereum timing:
external rotation remains sequential and may need to wait for its immutable
minimum epoch time; the immutable maximum lag may make it unavailable.
The internal terminal/next headers must satisfy the complete unchanged
verifier format/lineage envelope. No per-node wall clock changes BFT validity
or committee authority. Current Ethereum readiness is separately observed.

## C8. Proposed bootstrap and preactivation authority

All symbols below are **UNSET activation inputs**, not chosen mainnet
heights/hashes. A software binary alone cannot authorize them.

An approved future activation manifest fixes an admissible preactivation
V1 finalized checkpoint F and its fully verified old lineage. The old
signing set authenticated by that lineage is `Outgoing(F)`; it is not
selected from the new V2 candidate. It also fixes symbolic activation H,
version/config identities and the permitted transition procedure. A
checkpoint with broken/missing old lineage or unresolved incompatible
signature obligations is not assumed usable for this purpose.

The transition context is domain-separated, anchored to exact F and
`Outgoing(F)`, with one transition sequence. The outgoing keys run the
prepared/commit profile above to certify one `BootstrapStatement B0`:

```
F and old authority proof; network and fork/config identities; symbolic H;
exact preactivation parent P at height H-1; validated prefix F..P;
old current/next headers at P and every sequential intervening handover;
first V2 checkpoint context/current+precommitted-next authority;
FN roster from the finalized registry state at F;
V1 preserved-prefix manifest and cutoff identities;
initial V2 state roots, import cursors and version-routing rules.
```

P need not pretend to be an already V1-finalized block. The verified outgoing
transition CommitQC and the approved activation rule authenticate this exact
preactivation parent for V2. Every F..P block is validated as V1, and old
signer obligations constrain whether each signer may support B0. This new
transition certificate is not an old V1 finality certificate and cannot be
submitted to Ethereum. Its new digest never reinterprets a stored V1 vote.

The first V2 block must extend exactly P and carry B0/its transition proof.
A competing preactivation parent P' is not accepted for activation just
because it arrives first, is taller or appears in a local snapshot. A valid
alternative transition decision is a safety incident. During the transition
protocol, views can choose a different otherwise valid P only through C4
before decision and while respecting retained V1 obligations; H remains
fixed by the activation manifest. No real instance is started here.

V1 may have sequentially advanced epochs between F and P. B0 must prove
each such existing handover and preserve its committed successors; it
cannot replace an already-fixed `Set_(e+1)` with a convenient online subset.
The first V2 checkpoint instance inherits the exact current/next checkpoint
headers established at P. The transition quorum is still Outgoing(F),
fixed outside B0; verification never uses B0's proposed incoming keys to
authorize B0. Eligibility of that outgoing transition authority and a
usable signing quorum are activation prerequisites, not conclusions drawn
from the existence of an archive or a deployable binary.

B0 fixes the first B3 checkpoint state's `e/current/next` to the fully
validated V1 state at P and `epochFloor` to that state's last epoch-start
height. Define `G = highest validated V1 finalized checkpoint in V1State(P)`;
G must equal or descend from F and have height at least F's. F is the
transition authority's trust anchor, **not permission to lower inherited
finality**. The first B3 instance's prior finalized bound is G, its
parentValueId is the canonical B0 statement hash (not a particular
transition-QC signer subset), and its first sequence is zero. First V2
checkpoint height must be at least H, extend exact P, and satisfy C2's
grid/depth checks above G. Preserve every own-signing, ancestry-lock,
included/exported-certificate and Ethereum-acceptance high-water record;
none is reset to F, G or zero by installing B0. Later known incompatible
obligations still block signing under C2/C9. An already `lineage_broken`
state at P does not qualify through relabelling it V2. Subsequent epoch
floors follow C7.

Initial FlowMesh anchor is exact F, initial import cursor is end-of-block F,
and initial sequence is zero with the same authorized B0 parent identity.
This deliberately lagged FlowMesh input anchor does not lower B3's inherited
finalized bound G or any signer/export watermark.
The cursor is not silently set to H−1 while its finalized anchor is lower.
After a later authenticated anchor advances, C6 scans the intervening
prefix, applies version routing to each event, and ignores pre-H V1 events
only as **V2 credits**, not as erased V1 obligations. Other initial economic,
action and replay components are explicitly empty under the new V2 domain;
the retained V1 records and external replay/nullifier stores are not reset.

The initial shared V2 economic state is empty of V1 spendable balances.
Its preserved manifest commits only authenticated historical **prefixes**
and liabilities, explicitly not a claim that no later V1 certificate,
signature, client instruction or withdrawal exists. Later valid V1 claims
continue under the transition companion; B0 does not annul them. Missing
known source history blocks the preservation gate rather than authorizing
zero balances. New V2 custody credits require separately authorized
post-H V2 deposits; an old balance snapshot is never a funding source.

On restart, reconstruct B0 from the same approved activation input and
verified chain/proof, then replay. If the stored B0 conflicts with the
verified one or its prerequisite records are absent, fail closed. Do not
bootstrap a second V2 history from whichever peer responds first.

## C9. Sequential legacy export recovery, without old-lock erasure

Keep separate records for BFT decisions, verified legacy lineage, own V1
signing watermark/ancestry lock, legacy certificate inclusion/export and
Ethereum acceptance. Completing the same decided checkpoint's legacy
certificate is allowed by its dedicated pipeline record; no different
same-height object is authorized. Caught-up B3/FN state is not export-ready.

Current V1 `CommitSignedCheckpoint` permits same-epoch or the direct locked
successor, while `CommitCertifiedAnchor` requires a newer certificate under
the exact locked epoch/set. Therefore merely changing a cursor to e+3 is
not a supported V1 recovery operation. No existing RPC is claimed to do it.

Proposed V2 recovery adds a **separate authenticated lineage evidence cursor**:

1. Recover the actual durable own-signature/lock records and establish their
   freshness under C5. Never fill a missing record by guessing from peers.
2. From the exact recorded set/successor, verify each sequential handover,
   full set header/membership commitment, qualifying legacy certificate,
   inclusion/branch evidence and increasing checkpoint object. No skipped
   epoch, altered successor or quorum reduction. V2-era evidence additionally
   carries the corresponding internal decision and C7 transition.
3. Persist each verified evidence step without claiming the local key signed
   it and without lowering, deleting or relabelling any old watermark/lock.
4. Before any new export, prove its branch descends from the retained own
   ancestry lock (or that the existing independently valid V1 same-epoch,
   same-set newer-included-certificate recovery predicate already applies).
   Prove the target authority is reached through the complete evidence
   chain and all local key/height/digest rules still hold.
5. Only a separately audited V2 signer-store transition can consume that
   evidence to permit a new signature. Exact old records remain immutable
   history; no synthetic intermediate *local signatures* are generated.

This is proposed new evidence-driven recovery logic, not an instruction to
call V1 setters successively until they accept. Failure of any link reports
the missing/conflicting epoch/object and leaves export blocked. A certificate
under e+1 alone is not an unlock for an orphan signed in e. A visible branch
with too few votes does not prove that the orphan has no retained quorum.

Likewise, inherited `lineage_broken`, an expired deployed verifier or a
genuine split in old final signatures is not healed by returning online.
No emergency reset or artificial certificate is defined here. Any proposal
to change these historical validity outcomes is a separate recovery design
and owner/security decision. Offline quorum, missing data, retained orphan
lock, missed sequential lineage and conflicting full certificates remain
distinct statuses with distinct evidence.

## C10. Proposed acceptance cases and remaining approval gates

Before implementation approval, freeze codecs and exact synthetic model
configuration; review this candidate separately. Required cases include:

- Same body certified by different signer subsets has one ValueId/parent.
- Every missing guard in C3 rejects independently, including stale view,
  unscheduled leader, absent durable proposal, missing NEW_VIEW and missing QC.
- Hidden commit quorum; only one honest replica knows preparation; omitted
  local PreparedQC; delayed old decision; equivocation and withheld data.
- Timeout/online-list changes cannot authorize voting or roster changes.
- Different provisional tips with identical witnesses produce identical
  public validity; local export/old-lock failures are reported separately.
- 12-deep validity versus 20-deep export wait; later carrier is acyclic;
  different carrier inclusion heights preserve checkpoint authority while
  producer timing is verified under the proposed separation.
- Anchor/import lag, duplicated deposit, duplicate settlement, incomplete
  event prefix and pre-H V1 deposit discovered after H.
- Restart at each intent/signature/QC/application boundary; known rollback
  blocks signing; coherent undetectable rollback is not falsely reported
  detected by the stable-storage model.
- Bootstrap from authorized F/P/B0 versus first-seen snapshot, self-selected
  initial roster, competing preactivation parents, old precommitted next set
  and subsequent valid V1 claims not represented by the preserved prefix.
- Missed epochs recovered by full evidence without fake local signatures;
  missing link, stale own records, orphan lock, broken lineage and expired
  external verifier all fail with distinct reasons.

Unapproved choices: D1 protocol/fault/deployment freshness profile; D3
checkpoint/producer separation and checkpoint snapshot timing; D5 inherited
export lineage and its evidence-driven signer-store extension; D10 transition
authorization/cutover profile. FN epoch cadence, proof/queue bounds and
timeout values require configuration/vector freeze. The first isolated
accounting model need not select these consensus parameters or claim any
of these qualification results.

## Commit-pinned V1 evidence

All links below use inspected public V1 revision
`0b930e303e4c2c6bc28beb3bf656488b636ad49d`, not a moving branch:

- [Certificate schedule, depth and branch validity](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/modern/finality_schedule.h#L21).
- [Separate local 20-block signing policy](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/finality_signing_policy.h#L15).
- [Inclusion-derived rotation and successor snapshot](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/finality_tracker.cpp#L123).
- [V1 proposal/PREPARE/COMMIT guards and durable startup](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/flowmesh_agreement.cpp#L629).
- [Prepared/new-view proof checking](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/flowmesh/preagreement.cpp#L110).
- [Legacy signed-checkpoint lineage and certified-anchor restrictions](https://github.com/B3-Coin/B3-CoinV2/blob/0b930e303e4c2c6bc28beb3bf656488b636ad49d/src/node/finality_signer_store.cpp#L425).

These sources motivate differences; they do not implement or prove this
candidate. There were no live signing, activation or recovery actions.

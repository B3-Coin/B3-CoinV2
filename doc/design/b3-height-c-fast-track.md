# Height C: what can be built before DEX-1

**Status: PROPOSAL, no governing authority.** Written 2026-10-26 for the
project owner, against `r0-rebirth` at 2390ff02. It sets no height, rule or
parameter. Every consensus item below waits for an owner ruling. B0, though
not a consensus change, is `OWNER-GATED` in the ledger
(`src/flowmesh2/TODO.md:133`, `:267`) and is therefore designed here, not
implemented.

## 1. Why C is scheduled for months 10 to 15

The plan calls the two activations independent: "**P** activates FlowMesh
V2 on B3. **C** activates the base-chain finality/PoS improvements. They
are independent" (`doc/design/b3-flowmesh-greenfield-plan-20261016.md:45`).
It also says "M10 does not gate P" (`:975`). C is late for two other reasons:

- D8 puts DEX-1 (C7) before C5 "on regtest trusted facts" (`:1072`);
- D26, the PoS V2 scope ruling, is Tier 3: "after DEX-1, before the mainnet
  milestones" (`:1088`, `:1101`).

So C can move earlier without touching the DEX path, if the work is done
in parallel and D26 is ruled early. This document lists that work.

## 2. What C must deliver

| Source | Requirement |
|---|---|
| Plan §3.4.2 (`:373-377`) | Finality V2: "lock only on an observed quorum; never finalize where you nullified; per-block finality". PoS V2: "fix the stuck producer set and the headcount Sybil". No slashing. |
| [CC] C5 (`b3-flowmesh-clean-core-20261016.md:86`) | "Native authenticated inputs, immediate invalidation, bounded finality undo and required PoS V2 improvement". Exit: "Reorg/epoch/restart/full-replay comparison and safe membership/bootstrap; producer election is not silently left as completed V2". |
| Plan M10 (`:967`) | "Sign-versus-reorg linearization proved in the spec before native signing; producer election not silently declared done; U == U′". |
| Ledger | `B0-B3D-01` bounded undo (`TODO.md:267`), `M10-B3D-01` Finality V2 ("sign-versus-reorg proof first", `:376`), `M10-B3D-02` PoS V2 ("shadow fork; U == U′", `:377`), `M10-SPEC-01` (`:378`). |

B0 is "part of C5" and is scheduled for months 0 to 1.5 (plan `:956`).

## 3. What exists, and the gaps

| Requirement | What exists in this tree | Gap |
|---|---|---|
| B0: bounded undo of derived state | Bindings, FN seats, vaults, checkpoints and the bridge state undo one block in place. Both stake trackers and the finality tracker rebuild from H+1 on every disconnect (`src/validation.cpp:5118`, `:5138`). Nothing persists across a restart. Cost measured: about 18 µs per modern block per rebuild, about 0.8 s per one-block reorg on mainnet's length today, a floor (`~/B3-evidence/B0/report.md`). The 97 s gap was a regtest journal "around a shallow reorg" (`b3-flowmesh-redesign-plan-20261015.md:56`). | Undo for the three rebuilding trackers (§4). Persistence for restarts. |
| PoS V2: stuck producer set | H1 (regtest knob): producer epochs rotate from stake snapshots without certificates, with an escape after R_escape rounds. The V2 design adopts it: "P3-R1 producers … H1 schedule and escape (V2-D3)" (`b3-bridge-v2-finality-dependency.md:484`, `:507`). | The escape gives no liveness against one adversarial member in force (§5). Producer election cannot be called done until that is ruled. |
| PoS V2: headcount Sybil | A rule is designed: Committee(S) = members with weight ≥ W(S)/128, at most 64 (`b3-bridge-v2-finality-dependency.md:551-556`). "B-22 needs explicit owner approval" (`:557`). | Not implemented: "B-22 is out of scope" (`:912`). |
| Finality V2 (P3) | The fresh lineage exists on regtest: genesis at R_f, fresh keys, digest V2, transport (`finality_fresh_*` tests). It signs with "The V1 single-vote signer, unchanged in logic" (`:419`). | P3 itself, the voting that locks only on an observed quorum, has no code in b3d. |
| Sign-versus-reorg proof | `M10-SPEC-01` is written for FlowMesh seat authority (`TODO.md:378`). `M10-B3D-01` asks for a "sign-versus-reorg proof first" for Finality V2. | No proof for either. A bounded model of P3 against reorganizations at one checkpoint height now exists (§9). It is evidence about Stage 0's text, not the proof the rows ask for. |
| U == U′ | `b3coin-utxo-verify` compares the UTXO set at H (`src/bitcoin-utxo-verify.cpp`). | Plan §6.6's extended form, covering every tracker, does not exist. Shadow-fork runs need a mainnet copy, so they are owner-run. |

## 4. B0 design (proposed, not implemented)

### 4.1 Scope

Only the three trackers that rebuild on a disconnect change. The others
already undo in place and are left alone, except that the new tests also
check them.

### 4.2 B0a: the reorg path

- **StakeTracker** (`src/node/stake_tracker.{h,cpp}`). `ApplyBlock` records
  per block the entries it spent and the outpoints it created. A new
  `BlockDisconnected(index)` reverses the last block when the tracker is
  synced to it, and otherwise marks itself dirty, as today. Both
  instances get it: Chainstate's (eligibility) and the finality tracker's
  own.
- **FinalityTracker** (`src/node/finality_tracker.{h,cpp}`). Each connected
  block pushes the `State` it replaced. `State` holds the validator sets
  by `shared_ptr`, so the copy is small: one vector of epoch starts, the
  optionals and a few flags. `BlockDisconnected` restores that state, steps the
  stake and binding trackers back, clears the tip-snapshot memo and moves
  the synced tip to the parent. In fresh-key mode (regtest only) it keeps
  today's rebuild.
- **Bound.** The undo history keeps the last `reorg_horizon` blocks (1,440
  on mainnet), well under 2 MB for the finality state. A deeper
  disconnect, a gap or a restart falls back to today's full rebuild, which
  stays as the reference.
- **Call sites.** `src/validation.cpp:5118` calls the stake tracker's
  `BlockDisconnected` instead of `MarkDirty`. `:5138` is unchanged in form.

Validity does not change. The tracker returns the same state as a rebuild,
only sooner. A mismatch would split the node from the network, which is
why acceptance is undo equal to replay.

### 4.3 B0b: the restart path (outline)

Persist each tracker's state at one block at least `reorg_horizon` below
the tip, keyed by that block's hash. On start, load it if the block is on
the active chain and replay forward. That is at most a horizon plus the
blocks since the save, and it refills the undo history. This needs a
canonical serialization of `State`, the stake map and the indexes. It is a
separate design once B0a lands.

### 4.4 Files (B0a)

`src/node/stake_tracker.{h,cpp}`, `src/node/finality_tracker.{h,cpp}`,
`src/validation.cpp` (one call site), a new `src/test/derived_state_undo_tests.cpp`
and its entry in `src/test/CMakeLists.txt`.

### 4.5 Acceptance, failing first

1. A rebuild counter on each tracker. A one-block reorg must not rebuild
   the stake or finality tracker. This fails today.
2. Undo equals replay. For each depth up to the regtest horizon, after
   epoch boundaries, a certificate, an H1 rotation and an escape, the
   tracker after undo equals a tracker rebuilt from scratch. The same
   check covers the four trackers that already undo in place.
3. Cost. The one-block reorg time stays flat as the chain grows, measured
   with the B0 script.
4. Mutants: dropping one undo entry, or restoring the wrong state, fails
   test 2.
5. Owner-run: the same equality on a shadow copy of mainnet.

## 5. The escape rule (V2-D3)

`finality_h1_tests/escape_race_*` and
`test/b3_h1_escape/escape_race_model.py` show the following. All modern
blocks carry equal work, and at equal height the branch whose first block
is earlier wins. An escape block is always later than a non-escape sibling,
and nothing refuses a late block. So with every honest member in force
offline, one adversarial member in force holds the chain to one block per
R_escape rounds until rotation: about 4 to 8 years on mainnet. A withheld
in-force branch also replaces honest blocks, up to the reorganization
horizon.

`--compare-fixes` (2390ff02; evidence `~/B3-evidence/h1-escape-race/fix-compare/`)
reruns the search under three candidate rules. The bounds are small, and
E=1 makes every epoch one block, which hides the cumulative rule.

| Rule | Slow-producer attack | Withheld-branch reorg | Cost |
|---|---|---|---|
| Current | Height 14 in 22 steps (E=2), 18 in 36 (E=3) | Up to 3 honest blocks | — |
| Escape beats a non-escape sibling | Unchanged | Unchanged in 2 of 3 bounds: the adversary makes its own escape sibling at the same time and the grindable hash decides | Small fork-choice change |
| Refuse blocks published late | Unchanged | 1 block | Depends on each node's clock; nodes can disagree |
| Count the escape over the epoch | Height 18 (E=2), 30 (E=3) | Unchanged | Schedule carries one more number |

**Recommendation for prototyping, not a ruling.** Count the escape over
the epoch. The attacker's slack becomes R_escape rounds per epoch, at most
24 h added to an epoch of 1,440 blocks on mainnet, instead of 24 h per
block. New stake then enters by ordinary rotation within E to 2E blocks.
Withheld-branch reorgs are better bounded by Finality V2 than by a clock
rule: P3's committed checkpoint is the lower bound for fork choice
(`b3-bridge-v2-finality-dependency.md:464-467`), so a reorg cannot reach
below it. A tie-break on the eligibility digest instead of the block hash
would also stop grinding, since the digest does not cover block contents
(`src/modern/pos_v1.h:64-72`). That idea is untested.

## 6. A label to reconcile

Plan §3.4.2 says C "Requires C-12 (CLAUDE.md rule 3) and D3/D4 rulings"
(`:376`), and D26 reads "C-12 / D3 / D4 (PoS V2 scope)" (`:1101`). The
plan's own D3 and D4 are the stable-quote commit and the matching rule
(`:1067-1068`). The dependency document's V2-D3 is "PoS V2 scope" (`:507`)
and V2-D4 the signer incentive. D26 most likely means V2-D3 and V2-D4.
This is reported, not resolved.

## 7. What the owner would need to decide

1. Authorize `B0-B3D-01` as a review branch, starting with B0a.
2. Allow C5 work (PoS V2 and Finality V2) on a regtest-only review branch
   before DEX-1, and rule D26 (C-12, V2-D3) early enough to build against.
3. Choose the escape direction to prototype (§5).
4. Approve or replace the bounded committee (B-22, FD-14).
5. Say who builds P3, and whether its sign-versus-reorg proof comes first
   as a model, as `M10-B3D-01` asks.
6. Confirm which D3/D4 the C rows mean (§6).
7. Rule whether a valid P3 decision outranks the reorganization horizon for
   the headers that lead to it (§9.3).
8. Choose P3's validity rule: Stage 0's (any valid branch), or the hybrid
   of §9.4 (a fresh proposal must be on the voter's own chain).

## 8. Proposed order

1. B0a, with the tests of §4.5.
2. A model of P3 sign-versus-reorg, in the style of the escape model. Done
   for one checkpoint height (§9). A handover model is not planned, since it
   would hold by construction (§9.5).
3. The chosen escape rule on regtest, with the `escape_race_*` cases
   turned into acceptance tests.
4. The bounded committee.
5. P3 on the fresh lineage.
6. B0b, then extended U == U′ tooling for the owner's shadow-fork run.

## 9. Finality V2 (P3) against reorganizations: the model

`test/b3_p3_reorg/p3_reorg_model.py` searches every reachable state within
its bound. Evidence: `~/B3-evidence/p3-reorg/`. There is no P3 C++, so the
results are about the protocol text: Stage 0 §7
(`b3-flowmesh-v2-stage0.md:345-430`, `:547-581`) and R1
(`v2-stage0-r1-consensus.md:47-58`, `:118-140`). V1's ancestry lock
(`src/node/finality_signature.cpp:607-680`) runs beside it as the positive
control.

### 9.1 What is modelled

- Four members, equal weights, and {4,1,1,1} for NC2. The quorum is
  headcount ⌊2n/3⌋+1 and weight ⌊2W/3⌋+1.
- One checkpoint height with two candidates, A and B. Both extend the last
  committed checkpoint. Fork choice ignores votes, so any honest node's tip
  may move between them at any step.
- P3:
  - single-sequence PBFT-style phases;
  - one PREPARE per member per view;
  - prepared only on an observed quorum;
  - COMMIT only in the member's current view, so a member never finalizes
    in a view it has left;
  - a timeout reports the highest prepared value with its proof;
  - a new leader must re-propose the highest prepared value in a quorum of
    reports.
- Validity is either pure (Stage 0: a candidate with its branch data is
  valid on any tip) or the tip variant (vote only for a candidate on one's
  own tip).
- Faults:
  - a Byzantine member counts as having sent every vote and report it can
    back with a real quorum;
  - crashes are crash-stop.
- Progress (L1) is checked after stabilization:
  - no further faults or Byzantine messages;
  - tips settle on a branch the adversary picks;
  - fresh views follow.
- Bound: three views, or two for the scenarios with four honest members,
  with tips in the state, or with the private branch.

### 9.2 Results

| Scenario | Agreement (S1) | Progress (L1) |
|---|---|---|
| PC1: V1, two of four sign A, B wins (811,631) | — | Stalls |
| PC2: V1, one orphan lock plus one crash (814,191) | — | Stalls |
| P3, one crash, no Byzantine member | Holds | Holds |
| P3, one Byzantine member as view-0 leader, view-1 leader, or never leader | Holds | Holds |
| P3, tip variant, with or without a Byzantine member | Holds | **Stalls** |
| NC1: P3 with two Byzantine of four | Violated | — |
| NC2: weight-only quorum, heavy member Byzantine | Violated | — |
| NC2: dual quorum, heavy member Byzantine | Holds | Stalls, beyond the fault bound |
| Planted bugs (the new view ignores prepared reports; reports forget the prepared value; both thresholds one short) | Violated, each | — |
| Private branch C, Stage 0 validity, Byzantine view-0 leader | Holds; **C can be decided** | Holds |
| Private branch C, tip variant | Holds; C never decided | Stalls |
| Private branch C, hybrid (§9.4), Byzantine view-0 or view-1 leader | Holds; C never decided | Holds |
| Hybrid, four honest members | Holds | Holds |
| Planted bug: the hybrid accepts a "forced" claim without its proof | Holds; C can be decided | Holds |

The controls and planted bugs show that the checks can fail.

### 9.3 Findings

1. **P3 as written removes V1's orphan lock.** A prepared quorum on A is
   carried into every later view, or it is freed when the reporting quorum
   holds no prepared value. No member is locked by its own vote. The search
   found no reachable state within the bound that blocks progress once a
   quorum is live.
2. **That depends on validity not reading the voter's tip.** Under the tip
   variant, a prepared quorum on a block that B3 then abandons is V1's
   orphan lock again: each new leader must re-propose it, and the members
   now on B refuse it. All four members can be honest. R1 already says that
   receiving a valid candidate "can be PREPARE/COMMIT eligible" whatever the
   voter's tip (`v2-stage0-r1-consensus.md:124-132`). The model shows that
   this rule is load-bearing, so it should be an acceptance test of the C++.
3. **P3 may decide the branch that fork choice left.** B3 must then
   reorganize back to the decision, which Stage 0 intends
   (`b3-flowmesh-v2-stage0.md:576-581`). P3 does not bound how late a node
   learns a decision, and b3d checks the reorganization horizon
   (`src/validation.cpp:7042`, 1,440 blocks on mainnet) before the finality
   pin (`:7050`). A node whose branch forked 1,440 or more blocks below its
   tip when it learns the decision would refuse the headers leading to it.
   If it also applies the pin, it refuses new blocks on its own branch too,
   and follows neither. The dependency document finds the same shape for
   late upgraders (`b3-bridge-v2-finality-dependency.md:230`). Stage 0 does
   not order the two rules. That is a question for the owner (§7, item 7).

### 9.4 A private branch, and a hybrid rule

Stage 0's validity (finding 2) lets members vote for any valid branch,
including one that no honest node's fork choice ever selected. The model
adds a third candidate C: a branch the adversary built and withheld. Every
block of C is valid, but no honest tip is ever on it. Under Stage 0's rule,
a Byzantine leader proposes C in its view and the honest members prepare
and commit it (six steps, `~/B3-evidence/p3-reorg-private/`).

B3 must then reorganize every honest block above the last decided
checkpoint onto C. V1 could not be used this way, because members signed
only their own tip, and fork choice moves only to a branch with more
blocks. Under P3 the attacker needs a turn as leader, not the heavier
chain. The protocol itself limits the attack in only two ways:
- C must extend the last decided checkpoint;
- C must carry 12 blocks above its checkpoint (`v2-stage0-r1-consensus.md:100-104`).

So in practice the attacker needs a deep enough private branch before the
next decision moves past its fork point. That is plausible with a large
stake, or while P3 is stalled, for example during a partition. Anyone
who accepts B3 payments on confirmations, not on P3 decisions, is exposed
for that window.

The **hybrid** rule, in the style of Tendermint's lock rule, closes this
in the model without bringing back finding 2's stall:
- a member PREPAREs a fresh (free) proposal only if it is on its own tip;
- it PREPAREs a value that a verified NEW_VIEW forces, or COMMITs one it
  holds a prepared quorum for, on any tip.

C then cannot gather a first prepared quorum, so it can never be forced.
Progress holds, because a forced value is still accepted everywhere.

The cost: a fresh decision needs a quorum of members whose fork choice
agrees long enough. That holds whenever B3 converges, but it ties finality
liveness to fork-choice convergence, which Stage 0's rule avoids.

Within the bound, tips move freely, so the tip and hybrid rules reach the
same states. They differ only after stabilization, where tips stay put.

The hybrid contradicts Stage 0's text: "Proposal validation includes
required branch data, not a requirement that every replica already
selected the same provisional tip" (`b3-flowmesh-v2-stage0.md:552-553`).
So it is an owner choice (§7, item 8). The planted bug shows that the
protection depends on members verifying the NEW_VIEW proof before treating
a value as forced: taken on trust, C is decided again.

### 9.5 Not covered

- Consecutive checkpoints and the epoch handover. Under C7 the next
  instance's authority is a function of decided values only:
  - the incoming set is the one precommitted at the previous terminal
    decision;
  - the following set is the snapshot at the decided terminal
    checkpoint h;
  - the next floor is h+1 (`v2-stage0-r1-consensus.md:332-343`).
  Every honest node therefore derives the same authority, whatever
  branch it is on. A model of that would hold by construction. Its only
  content would be a V1 control for the inclusion-height ambiguity, which
  C7 already states (`:324-330`). Liveness at the handover still needs a
  live quorum of the precommitted incoming set and an included carrier.
  The first is the inherited lineage-break risk (RC5, FD-7). The second
  needs a producer to include it, which H1 provides.
- H1 producers.
- The Ethereum verifier and digest V2.
- The bounded committee (NC3).
- Amnesia, except as the planted bug.
- Message loss beyond what "any subset may be received" already gives.

A pass is evidence about the specification within the bound, not about any
implementation.

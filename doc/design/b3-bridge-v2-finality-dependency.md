# B3 Bridge V2: finality dependency, H1 and the fresh lineage

> **Status: PROPOSAL, no governing authority.** It ranks below every owner
> ruling and every reviewed, committed governing document. Nothing here is
> ratified, built, model-checked, audited or activated.
>
> - 2026-10-03, revision 9 (cross-document fixes). Base `b4b1de4`, branch
>   `feat/flowmeshV2-dev-claude`. Nothing built or run; no node, chain RPC or
>   signer contacted. Follows the owner's 2026-10-03 direction as relayed in
>   [b3-bridge-v2-arbitrum-proposal.md](b3-bridge-v2-arbitrum-proposal.md)
>   (the "architecture document").

**Labels.** [E] established technique or verified fact; [I] design choice;
[H] estimate or unverified. **Default** means a provisional solve-and-report
choice, open to owner ruling. Repo facts are `path:line` from the worktree
root; external facts are URLs; `.../study/` is
`/Users/josh/development/ON/B3-FlowMesh-200ms-evidence/arbitrum-bridge-study-20261003/`.

**Scope.** This document owns B3's finality repair (§4), **H1** (§5), the
**fresh lineage and re-bootstrap** (§6), B3's source for each digest V2 value
(§7), the regtest staging signer (§8), **V2 P3** (§9), what a lineage failure
costs the bridge (§10), the finality side of bootstrap (§11) and the model
check (§13). The contracts spec is normative for every byte layout. Ids
follow the architecture document: **B-n** (its §14), **S0-S9** and
**RT0-RT10** (§13), **O-n** (§17); its §7 owns every minute figure.
Sub-decisions here are **FD-n** (§15).

---

## 0. Short version

1. **B3 needs H1 for itself.** Under V1 producers rotate only after a
   finality certificate. Mainnet's lineage is expected broken since about
   826,841, so producers are frozen at Set_3 for good [H; O2]: new stakers
   never produce, an unstaked member produces forever, and no Set_3 key
   online halts the chain, permanently only if those keys are lost or never
   return. H1 is a height-gated hard fork rotating producers every 1,440
   blocks, with a 24-hour **escape** so an offline producer set cannot halt
   the chain; no BLS, bridge or Ethereum code. Expedite it if O2 shows few
   Set_3 keys producing (S1, B-6).
2. **Reorg protection.** Online nodes keep the 1,440-block reorg limit. A
   fresh node is protected only to the hardened checkpoint 811,641 until it
   validates the block carrying the 814,051 certificate, so a hardened
   checkpoint (**H2**) is mandatory in the H1 release.
3. **The bridge does not drive H1 but is ordered after it, and has no
   deadline.** The old verifier lapses 2026-10-08T17:11:59Z and nothing is
   owed to it (B-1, B-2). On mainnet the bridge opens only after V2 P3
   exports digest V2 (B-7).
4. **Fresh lineage (B-8).** A re-bootstrap at R_f with its genesis committee
   from Snapshot(R_f − 1), a new lineage id, new tags and a new journal
   namespace, so old orphan locks cannot block it and no old signature
   validates under it. The V1 spec anticipated this
   (`doc/design/b3-cross-chain-finality-v1.md:175`).
5. **Digest V2 (B-9)** binds the lineage id and a checkpoint-history root, so
   a verifier rejects a certificate that does not extend its last accepted
   one. Mainnet still needs an exporter that never exports a checkpoint B3
   can later abandon, and never two conflicting quorum certificates. V1
   voting already exported an orphan: 811,631's certificate reached Ethereum
   (`doc/release-notes-v1.1.3.md:100-106`). That is P3's job.
6. **A lineage failure strands the reserves.** Each vault is bound for ever
   to one verifier and one lineage. A B3-side break, a 30-day expiry, a
   conflict halt or a permanent quorum loss locks every token not already in
   an accepted root for good, out of a re-bootstrap's reach. Hence: a B3 break
   deadline later than contract expiry (FD-7); a bounded committee against a
   headcount Sybil that today costs about 17 keys of 333 B3 (FD-14, S5 gate);
   and a signer-liveness program (§10.4).
7. **Regtest first.** Before P3, the V1 single-vote signer signs digest V2
   behind a regtest-only gate (RT4); P3 later replaces only the voting (RT8).
8. **Latency** (architecture §7): withdrawals arrive in 29-50 min with
   prompt relays (29-47 for (d1)/(d2)), up to about 30 min later when
   Ethereum relays are batched above 1 gwei.

---

## 1. V1 finality and why it stalled

All [E].

- **Vote.** One BLS vote per member per checkpoint, `(h − M) mod 10 = 0`
  (`src/modern/finality_schedule.h:61-64`), over a 112-byte object tagged
  `B3/FINALITY/V1` (`src/modern/finality_types.h:64-107`, `:294-300`).
  Consensus depth 12; wallet export waits 20
  (`src/kernel/chainparams.cpp:293-294`; `src/node/finality_signing_policy.h:16`).
- **Journal.** Per (chain domain, key); the ancestry lock moves only on a
  newer included certificate of the same epoch and set, or a compiled pin
  (`src/node/finality_signature.cpp:607-680`).
- **Certificate.** Headcount ≥ ⌊2n/3⌋+1 **and** weight ≥ ⌊2W/3⌋+1; in a
  coinbase, depth ≥ 12, epoch e or e−1 (`src/modern/finality_schedule.h:132-172`);
  inclusion raises a reorg pin (`src/validation.cpp:2511-2548`).
- **Epochs and producers.** Epoch e+1 starts at the first height
  ≥ start_e + 1,440 with an epoch-e certificate below it; after
  1,440 + 10,080 blocks without one, `lineage_broken` is set for good
  (`src/node/finality_tracker.cpp:123-138`). Producers come from
  `SetInForceAt`, so they change only after a handover
  (`src/validation.cpp:2557-2578`); weight 0 is never eligible
  (`src/modern/pos_v1.h:109-115`).

**Root causes.** **RC1** a validator locks on its own vote, not on evidence
that the branch won (`src/node/finality_signature.cpp:607-618`). **RC2** the
only normal unlock is a newer certificate of the same epoch and set, which
may need the locked keys (`doc/finality-quorum-deadlock-814191.md:25-29`).
**RC3** fork choice ignores votes, and signing depth (12/20) is far below the
1,440-block reorg limit (`:31-36`), so an honest quorum can certify a
checkpoint B3 later abandons, as at 811,631 (`doc/release-notes-v1.1.3.md:57-62`).
**RC4** the V1 verifier checks no ancestry, and its first certificate of an
epoch fixes the successor (`contracts/src/B3FinalityVerifier.sol:194-288`).
**RC5** a break is permanent and the specified re-bootstrap was never built
(`src/node/finality_tracker.h:79-84`). A repository test verifies two
conflicting quorum certificates only under a proposed depth-20 fallback,
never under production guards
(`src/test/finality_depth_recovery_prototype_tests.cpp:254-258`); it stays a
positive control.

**Incidents:** 811,631, recovered by the one-time pin to 811,641
(`doc/release-notes-v1.1.3.md:57-106`); 814,191, whose best later checkpoint
had 21 signers against 23 needed (`doc/finality-quorum-deadlock-814191.md:16-20`);
then the expected break near 826,841 [H].

| Cause | H1 (S1) | Fresh lineage | Digest V2 + verifier V2 | V2 P3 (S5) |
|---|---|---|---|---|
| RC5 freezes producers | **Removed** (schedule plus escape) | — | — | — |
| RC5 ends finality | — | **New start** | — | Break deadline stays (FD-7) |
| RC2 old orphan locks | — | **Irrelevant** (new namespace) | — | Quorum-evidence unlocks |
| RC1, RC3 | — | — | Rejects only an orphan that does not extend the last accepted root | **Removed**: committed checkpoint bounds fork choice |
| RC4 | — | — | **Rejected** by ancestry | Not produced |

---

## 2. Expected mainnet state

| Fact | Value | Label |
|---|---|---|
| Epoch starts | 811,001 / 812,441 / 813,881 / 815,321 | [I] derived |
| Set_2, pinned as successor on Ethereum | 34 members, weight 126,108,063; headcount quorum 23 | [E] `doc/design/v2-stage0-r1-bridge.md:221-223`; `.../study/check.md:29`; [I] quorum |
| Set_3, producers since about 815,321 | Snapshot at 813,880 | [H] until O2 |
| Last finalized; `lineage_broken` | 814,051; since about 826,841 | [H] until O2 |
| Hardened checkpoints; minimum chain work | 810,001 and 811,641; 0 | [E] `src/kernel/chainparams.cpp:326-329`, `:366` |
| Block spacing | Computed ~78 s; unmeasured | [H] computed in `doc/design/b3-finality-benchmark-2026-08-23.md:12-16`; owner read O8 |

O2 decides H1's urgency and adoption risk.

---

## 3. The old lineage lapses

- **No repair** (B-1). The old verifier refuses every certificate after
  2026-10-08T17:11:59Z (`contracts/src/B3FinalityVerifier.sol:211-213`);
  relaying 814,051 is optional, default skip (B-2).
- **B3.** Nothing is deleted. The V1 tracker keeps computing the old state
  for history and the legacy burn rule, which stays closed
  (`src/node/finality_tracker.cpp:46-63`; node changes §10); V1
  certificates stay invalid while the flag is set
  (`src/modern/finality_schedule.h:141`). Old journals, including
  814,191-era orphan locks, are kept and never consulted by the fresh lineage.
- **Why no V1 signature verifies under V2** [I]: the signed message differs
  in tag, length and lineage id (contracts spec §2.2). Separately, ruleset-2
  set headers stop a V1 header from bootstrapping V2
  (`contracts/src/IB3FinalityProver.sol:13`, `:64`). Keys are reused (§6.4),
  so the header rule is not a signature barrier.

---

## 4. Chain health: does B3 need a repair now?

**Yes.** With the lineage broken, V1's producer set never changes again:

| Area | Effect while frozen | Label |
|---|---|---|
| Blocks | Continue while one Set_3 key is online; empty rounds relax eligibility only for set members until it saturates (~240 s blocks for a 1/34 key). No Set_3 key online: halt; permanent only if those keys are lost or never return | [E] `src/modern/pos_v1.h:74-79`, `:109-123`; `src/validation.cpp:2574-2577` |
| New stakers | Never produce or earn | [E] `src/node/staking.cpp:669`; `src/test/finality_eligibility_tests.cpp:75-82` |
| Unstaking | Ordinary spend, no cooldown; a member can withdraw all stake, sell its key and keep producing | [E] `doc/design/b3-modern-pos-spec.md:23-24` |
| Reorg protection | Online nodes: 1,440 blocks below their own tip, not applied during import (`src/validation.cpp:6655-6666`). Fresh node: hardened 811,641 only, until it validates the 814,051 certificate's block and raises the pin from tracker state (`:2511-2535`) | [E] |
| FlowMesh | V1 runs, but reorgs of 31-1,439 blocks trigger its safety halt; V2 settles only on finalized anchors | [E] `src/node/flowmesh_anchor.h:17-41`; `doc/design/b3-flowmesh-v2-stage0.md:496-505` |

**Why not wait for P3** [E sources; I conclusion]: Stage 0's bootstrap needs
an intact old lineage and says a broken one "does not qualify"
(`doc/design/v2-stage0-r1-consensus.md:409-415`, `:468-469`); P3-R1 still
freezes producers when no carrier qualifies (`:370-376`); and a lineage that
producers follow would freeze them again after 11,520 blocks of quorum
failure (`src/kernel/chainparams.cpp:292-295`).

**Answer:** ship **H1 with H2** (S1, RT1) once reviewed and rehearsed,
before any mainnet fresh lineage; the bridge waits for P3, B3's health does
not.

---

## 5. H1 producer rotation

### 5.1 Rule [I]

From activation height R_h (unset everywhere until ruled):

| Item | Rule |
|---|---|
| Producer epochs | Start at R_h and every E = 1,440 blocks after the latest start; no certificate needed |
| Bootstrap | ProducerSet_0 = ProducerSet_1 = Snapshot(R_h − 1), as V1's Set_0 = Set_1 (`src/node/finality_tracker.cpp:107-120`) |
| Rotation | At the start of epoch k ≥ 1, ProducerSet_(k+1) = Snapshot(start_k − 1): V1's one-epoch lag, so a stake change takes force within E to 2E blocks (1.3-2.6 days at 78 s) |
| Snapshot | Unchanged: ACTIVE stake with a non-revoked FINALITY_KEY binding (`src/node/validator_set.h:28-32`) |
| Carry-over | Fewer than `min_finality_set` members: keep the previous set (`src/node/finality_tracker.cpp:72-83`); if Snapshot(R_h − 1) is too small, ProducerSet_0 = the V1 set in force at R_h − 1 |
| **Escape** | A block whose consensus-checked round (`src/modern/pos_v1.h:87-93`; `src/validation.cpp:6642`) is ≥ R_escape = 2,880 (24 h at 30 s rounds) takes its eligibility weights from Snapshot(parent) and starts a new producer epoch at its height with both sets = Snapshot(parent). Restarting matters: otherwise each later block would wait another 24 h |

### 5.2 Requirements

| # | Requirement |
|---|---|
| H1-R1 | Below R_h validity is byte-for-byte V1; reindex reproduces break, rotations and escapes |
| H1-R2 | One helper answers "producer set at height h" for validation, miner, staking loop, RPC and GUI (`src/validation.cpp:2574`; `src/node/miner.cpp:328`; `src/node/staking.cpp:445`, `:663-672`; `src/rpc/blockchain.cpp:3633`, `:3812`; `src/node/interfaces.cpp:1161`) |
| H1-R3 | V1 tracker content unchanged (checkpoint authority where a lineage is intact; legacy burn rule) |
| H1-R4 | Deterministic from block data only (heights and exact block timestamps); no wall clock or peer state |
| H1-R5 | Wallet and RPC report producer epoch, next rotation, escape status and when a new stake enters |
| H1-R6 | No change to BLS, digests, journals, carriers, bridge or Ethereum code |

An escape block cannot come early: a round's block is refused before its
exact timestamp minus 120 s (`src/validation.cpp:6646-6654`). Its race with
a late in-force block on the same parent needs the model check (§13) [H].

### 5.3 H2 hardened checkpoint [I]

Mandatory in the H1 release: a hardened modern checkpoint at a recent,
reviewed block in the existing list (`src/kernel/chainparams.cpp:326-329`),
refreshed each release until S5. It protects fresh nodes from a rewrite
after 811,641 by old Set_3 keys; it does not help online nodes against
reorgs under 1,440 blocks. A follow-up release should harden a block above R_h.

### 5.4 Ruled text and adoption

- **Overrides.** CLAUDE.md rule 3 (`CLAUDE.md:87-88`) and `CLAUDE.md:12-13`
  (no consensus change unless the owner asks after audit). The FINAL "one
  stake universe" identity (`doc/design/b3-cross-chain-finality-v1.md:172`)
  is broken at a given height, not only retimed: production and checkpoint
  weight come from different snapshots and, with FD-14, different functions.
  Handover-gated rotation (`:173`) ends for production.
- **Adoption.** Non-upgraded nodes follow a Set_3-only chain; upgraded nodes
  accept only Snapshot(R_h − 1) until an escape. The owner sets R_h after O2
  and operator coordination.
- **Late upgraders.** A node on the old chain that upgrades more than 1,440
  blocks after R_h cannot switch: the reorg horizon refuses the H1 branch and
  stored blocks are not revalidated (`src/validation.cpp:6655-6666`).
  Guidance: upgrade before R_h, otherwise run `-reindex` or
  `invalidateblock` on its first block after R_h. Exchanges need this notice.
- **Residual.** An unstaked member still produces for up to 2E blocks.

### 5.5 RT1 (S1), finality items

The regtest plan owns the harness, the knobs
`-b3regtestwithholdcertificates` and `-b3h1height`, and the fixture (epoch
40, interval 5, depth 3, extension 40, minimum set 4; its §3.6). Its RT1
carries all five finality items: (1) a broken V1 lineage keeps a newly bound
staker ineligible and a member that spent its stake eligible; (2) H1 rotates
at R_h, R_h + E and R_h + 2E, the new staker entering and the spent member
leaving on schedule, with carry-over below the minimum and reindex,
validation, miner, staking and RPC agreeing; (3) under an intact lineage V1
certificates stay checkpoint authority while producers follow the schedule;
(4) with all in-force producers offline an escape block after R_escape
restarts the schedule, as does activation with no upgraded member of
Snapshot(R_h − 1); (5) a node upgraded after R_h + horizon stays on the old
chain until `-reindex`.

---

## 6. Fresh lineage and re-bootstrap

### 6.1 Why fresh [I]

Unlike a resume, it needs no Set_3, orphan-lock pins or deadline, and the new
contracts can demand ancestry and a lineage id.

### 6.2 Genesis rule [I]

| Item | Default |
|---|---|
| Activation | Height-gated R_f in a reviewed hard fork (owner-set; regtest first) |
| Grid | V1's, `(h − M) mod checkpoint_interval = 0` (contracts spec §2.3 defers to this); R_f − 1 must be on it (mainnet: R_f − 1 ends in 1). R_f − 1 is leaf 0 and the contracts' `GENESIS_HEIGHT` |
| Genesis committee | Set′_0 = Set′_1 = Committee(Snapshot(R_f − 1)) with ruleset-2 headers; Committee is the bounded rule of §10.3 |
| Authority | The release itself, as V1's bootstrap at M (`src/node/finality_tracker.cpp:107-120`): "a consensus re-bootstrap pins a new genesis set (rule, not improvisation)" (`doc/design/b3-cross-chain-finality-v1.md:175`). No old-set vote |
| Lineage id | Contracts spec §2.2 with genesisHeight = R_f − 1, that block's hash and setHash(Set′_0) |
| Epochs, producers | Epochs restart at 0. Producers are unchanged by R_f (full snapshots, H1 schedule); the committee has its own epoch boundaries |
| Chain domain | Unchanged; regtest's differs [E] (`src/modern/chain_domain.h:24`) |

### 6.3 Parameters

E = 1,440, interval 10, depth 12, export wait 20, `min_finality_set` 2
(`src/kernel/chainparams.cpp:291-300`); bridge qualification 4-64 members and
weight ≥ 900 (B-22).

**Break deadline (FD-7), default changed.** V1's extension of 10,080 would
break the fresh lineage 11,520 blocks into a stall (8 days at 60 s), while
the verifiers accept the same committees for 30 days and a break strands
the reserves (§10.1). Default, as architecture §8.1 (B-23): **extension
≥ 46,080**, so E + extension ≥ 47,520 blocks: at least 33 days at the 60 s
floor (`src/validation.cpp:6646-6654`), the 30-day expiry plus 3 days for
relay delay and skew; about 43 at 78 s. The break then follows each
verifier's expiry, so a quorum that recovers while a verifier lives revives
the same contracts. Cost: a stalled committee keeps B3 checkpoint authority
for 33-43 days; producers are unaffected (H1), and burns close within about
2 h of a stall (node changes §8.4, C3). Alternative: no B3-side break. Either
is a new constant for the owner (`doc/design/b3-master-handoff.md:1160`).

### 6.4 Keys and journals [I]

- **Keys.** Existing FINALITY_KEY bindings are reused. One BLS key may sign
  differently tagged messages; proof of possession is checked at binding
  [E standard practice].
- **Journals.** A new store keyed by (lineage id, key). Old journals are
  never deleted, reset or read for new-lineage signing; no V1 vote can
  combine with V2 votes into anything accepted, so old locks protect only the
  dead lineage. Without the split, a key locked on the 814,191 orphan would
  refuse every new branch (`src/node/finality_signature.cpp:607-618`).

### 6.5 Tracker, carrier, transport and RPC [I]

- **Tracker.** Separate from producers; V1's handover-gated epochs under
  staging, P3-R1's under P3 (`doc/design/v2-stage0-r1-consensus.md:334-350`).
  Contracts see only epoch numbers and successor hashes.
- **Breaks (FD-8).** Not healed automatically. A ruled re-bootstrap makes a
  new lineage and needs new contracts; it **cannot release anything held by
  the existing vaults** (§10.1), so it is a stranding event, not a recovery,
  unless verifier V2 gains a succession path the live set signs before
  expiry (FD-15).
- **Carrier.** A new coinbase record version carries FinalizedBlockV2,
  bitmap and aggregate signature, judged like V1
  (`src/modern/finality_schedule.h:132-172`) plus lineage id, index, history
  root and ruleset 2; under P3 it also binds the CommitQC
  (`doc/design/b3-flowmesh-v2-stage0.md:592-597`).
- **Votes.** Staging votes need a message that names the lineage (the V1
  `finsig` has none, `src/net_processing.cpp:6636-6702`); under P3, Stage
  0's PBFT messages replace it.
- **RPC.** New `getfinalitylineage`: lineage id, R_f − 1 and its hash,
  committee headers and set hashes by epoch (ruleset 2, paged members),
  history frontier and latest index, the history path for any (index, leaf)
  from the block index (ancestry paths, `leaf0Path`, §11), and status
  (bootstrapped, broken, epoch, finalized). `getfinalityset` serves only V1
  sets (`src/rpc/blockchain.cpp:3707-3716`).
- **Code.** The `FinalityDigest` users (`src/modern/finality_types.h`,
  `src/node/finality_signature.cpp`, `src/node/finality_transport.cpp`,
  `src/bridge/bootstrap_proof.h` and others), the tracker and the
  accumulator; V1 paths stay for history.

### 6.6 Where the lineage starts

Regtest: S3 under the staging signer (RT4). Mainnet: S5 with P3 (B-7).
**Considered, not default (FD-10):** a mainnet interim lineage under V1
voting for B3's own pin. It would bring back V1's deadlocks and orphan
certificates; if wanted, it must use a lineage id that no bridge contract
ever accepts.

---

## 7. Digest V2: where B3 gets each value

Contracts spec §2.1-§2.3 defines every byte (lineage id, the 184-byte
FinalizedBlockV2, digest V2, the height-keyed history leaf, the root over
leaves 0..k−1, `certId`; no genesis statement), and this document restates
none. B3's sources [I]:

| Field | B3 value |
|---|---|
| lineageId | Fixed at R_f (§6.2) |
| height, blockHash | The grid checkpoint on the validating chain |
| withdrawalRoot | **Zero for height < V** (= `BRIDGE_ACTIVATION_HEIGHT`, node `activation_height`), then `FinalityWithdrawalRootV2(height)`, non-zero even when empty, as V1 (`src/node/bridge_state.cpp:1162-1171`; contracts spec §2.4; node changes §7.4). The burn height W2 (default V) is invisible to the contracts |
| validatorSetHash | keccak of the successor's ruleset-2 SetHeader |
| epoch | Fresh-lineage epoch of the signing committee |
| checkpointIndex | (height − (R_f − 1)) / checkpoint_interval (10 mainnet, 5 regtest), below 2^32 |
| checkpointHistoryRoot | Root over leaves i = 0..k−1, leaf i = historyLeaf((R_f − 1) + i·interval, blockHash_i) |

A history root rather than a previous-certificate pointer lets relayers skip
certificates (contracts spec §2.3). Every node keeps the 32-hash frontier per
chain position, with undo on reorg and rebuild on reindex, reusing the
withdrawal tree (`src/modern/withdrawal_tree.h:33-34`); consensus rejects a
V2 carrier whose index or root differs from its own, as V1 does for the
withdrawal root (`src/modern/finality_schedule.h:157-167`). **Shared
C++/Foundry vectors** for lineageId, historyLeaf, the root at k, a leaf-0
path and digestV2 go into RT4 and Stage 0's G1 vector freeze
(`doc/design/b3-flowmesh-v2-stage0.md:933-934`).

**What ancestry fixes, and what it does not.** It rejects a certificate that
does not extend the last accepted checkpoint (RC4), and with the
accepted-root history a release can prove against any accepted root at or
above its leaf. It cannot pick the canonical branch: an orphan certificate
that extends the last accepted root and arrives first is accepted, after
which the canonical chain is rejected; and any later conflicting certificate
can be reported to halt the verifier (contracts spec §3.4). The mainnet
requirement is therefore: **no exported certificate for a checkpoint B3 fork
choice can abandon, and no two conflicting quorum certificates.** V1 voting
fails the first (811,631), so mainnet waits for P3 (B-7).

---

## 8. Regtest staging signer

The V1 single-vote signer, unchanged in logic, signs digest V2 under the
fresh lineage with its own journal namespace and the V2 carrier, in a
"staging" mode that exists only in regtest params and that code refuses
elsewhere [I]. It makes the whole bridge testable end to end before P3. It
does not prove BFT safety. Within one committee, a reorg past a checkpoint
a quorum already signed yields an orphan certificate plus a deadlock (RC1-RC3).
Conflicting quorum certificates arise only across an epoch boundary, where
unlocked members of the next committee can certify the other branch, or with
amnesia; the regtest plan makes them with its §3.2 certificate tool
(`src/bridge/regtest_certify.cpp`).

**RT4 (S3), finality items** (in the regtest plan's RT4, except item 6,
which is regtest §6.2 RT9 fixture B):

1. Fresh lineage at R_f = 262, R_f − 1 = 261 on the 131 + 5k grid (regtest
   plan §3.6): fixture I first (V1 intact, no H1; H1 orders only the mainnet
   lineage, §4), then fixture B (H1 at 220). The genesis committee equals
   Committee(Snapshot(R_f − 1)).
2. A key with an old-journal orphan lock signs in the new lineage.
3. B3's V2 carrier rule rejects a V1-tagged signature, a ruleset-1 header
   and an other-lineage certificate.
4. Both anvil verifiers start from a staging certificate (epoch 0, and
   k ≥ 2) plus `leaf0Path` (§11), and the k-of-n fallback initializes;
   `getfinalitylineage` matches the pins; shared vectors pass.
5. Route (a) round trip; ancestry rejection; a release survives a newer root.
6. Stalled lineage: deposits close after 24 h without certificates, 72 h
   with keep-alives but no rotation; producers keep rotating (its RT9 row 1).

---

## 9. V2 P3

### 9.1 What P3 is (Stage 0, not redesigned)

- **P2 (V2-D1 pending):** single-sequence PBFT-style phases; a CommitQC is
  the decision (`doc/design/b3-flowmesh-v2-stage0.md:345-372`); every phase
  needs headcount **and** weight (`doc/design/v2-stage0-r1-consensus.md:47-55`);
  after PBFT
  (https://www.usenix.org/legacy/publications/library/proceedings/osdi99/full_papers/castro/castro_html/node4.html).
- **P3:** a B3-finality instance agreeing on checkpoint hash, withdrawal root
  and successor before any external signature; candidates extend the last
  committed checkpoint, which is the lower bound for fork choice
  (`doc/design/b3-flowmesh-v2-stage0.md:547-553`, `:576-581`).
- **Depth:** a 12-deep proposal can commit; export waits for the 20-block
  policy (`doc/design/v2-stage0-r1-consensus.md:124-132`).

### 9.2 Amendments the fresh start requires (B-20)

Stage 0 and R1 are PROPOSED, not ratified
(`doc/design/b3-flowmesh-v2-stage0.md:4-5`;
`doc/design/v2-stage0-r1-consensus.md:3`), so these are proposals to amend
proposals.

| Stage 0 text | Today | Proposed |
|---|---|---|
| Export (`b3-flowmesh-v2-stage0.md:547-553`, `:599-604`, `:709`) | Unchanged V1 signature; new external digest "Rejected" | Digest V2 under the fresh lineage |
| Ethereum row and R line (`:43`, `:69`) | Preserve the deployed bridge | Fresh contracts V2 (B-1) |
| More than 64 members (`:711`) | Keep a compatible set, or approve "a bounded, lineage-authorized bridge committee" | The bounded committee (§10.3) |
| C8 bootstrap (`v2-stage0-r1-consensus.md:404-480`) | Outgoing(F) of an intact V1 lineage certifies B0, which also fixes the FN roster at F (`:426`), initial V2 roots and cursors (`:428`), the prior bound G (`:458-464`) and the FlowMesh anchor "exact F" (`:472`); "A software binary alone cannot authorize them" (`:407`) | Split in two. (1) The lineage genesis Set′_0 is fixed by the release (precedent `b3-cross-chain-finality-v1.md:172`, `:175`). (2) B0 is the first value Set′_0 decides by P3, with F := G := R_f − 1 for the FN roster, the FlowMesh V2 anchor and import cursor and P3's prior bound; other B0 fields as C8, anchored at R_f − 1; FlowMesh V2's own activation height stays a D10 input. This contradicts `:407` and Outgoing(F): an owner ruling (FD-12) |
| C2/C9 obligations (`:466-467`, `:536-542`) | Old locks keep blocking signing | Scoped to the old lineage |
| P3-R1 producers (`:370-376`) | Rotate only after a carrier | H1 schedule and escape (V2-D3) |
| Breaks (`:536-542`) | Not healed by coming online; deadline undefined | Deadline per FD-7; re-bootstrap per FD-8, disclosed as stranding |

### 9.3 How P3 meets the bridge's needs

| Need | P3 | Remains |
|---|---|---|
| Timely certificates | Export follows each decision | Live quorum and timeouts (unselected) [H] |
| No exported checkpoint B3 can abandon | Export only after a durable CommitQC, and the committed checkpoint bounds fork choice (`b3-flowmesh-v2-stage0.md:576-581`, `:599-604`) | Holds while safety holds |
| No conflicting quorum certificates | Quorum intersection | Faulty below 1/3 by headcount **or** by weight (§13) |
| Epoch continuity | Next epoch needs a carrier with the prior CommitQC (`v2-stage0-r1-consensus.md:352-357`) | A missing export stalls the next epoch |
| Committee of 4-64, Sybil-resistant | Not addressed | FD-14 (S5 gate) |
| Producers never freeze | Only with the H1 amendment | — |

### 9.4 Latency and decisions

PREPARE and COMMIT fit in the 8 blocks (about 10 min) between depth 12 and
the 20-block export wait, leaving t_fin unchanged; a slower decision adds
its excess. Real-network PBFT round time is unmeasured [H].

| V2 decision (S5 gate) | Default here |
|---|---|
| V2-D1 protocol | PBFT-style P2 |
| V2-D3 PoS V2 scope | P3-R1 checkpoint authority; producers on H1 |
| V2-D4 slashing, inactivity | None; keep equivocation evidence; incentive is FD-16 |
| V2-D5 bridge authority | Bounded committee (FD-14) |
| V2-D10 transition | Fresh lineage at R_f, P3 from its first instance, C8 split (FD-12) |

---

## 10. What a lineage failure costs the bridge

### 10.1 Stranding [E contract facts; I consequence]

Each vault's `VERIFIER` is an immutable (contracts spec §6.1); each verifier
accepts only its immutable `LINEAGE_ID`, from the current or next set (§3.1,
§3.3 step 4); funds leave a vault only by release of a certified leaf (§10,
"Escapes"). Releases against already accepted roots continue after expiry,
halt or cancel. Everything else in the vault, the backing of all outstanding
bUSD drawn on it, can then never leave:

| Event | Vaults stranded |
|---|---|
| Fresh-lineage break on B3 (FD-7) | Both |
| A verifier's 30-day expiry | That vault |
| Conflict halt (`reportConflict`; one certificate is valid on both chains), even after the canonical certificate landed first | Each chain where it is reported |
| Permanent quorum loss, including the Sybil halt (§10.2) | Both, at break or expiry |

Burns close first (node changes §8.4), so holders cannot exit through B3
either. Architecture §8.1 and §12, node changes §8.7 and the README's risks
say so; regtest plan §6.2 rows 1b, 3 and 12b drill the break, expiry and
Sybil halt and record the stranded amounts.

### 10.2 Who can do what

| Faulty share | Can do | Label |
|---|---|---|
| About n/2 extra keys at 333 B3 each, withholding (full-snapshot committee) | Halt for good: 17 keys (5,661 weight, about 0.005% of Set_2's 126,108,063) raise Set_2's headcount quorum to 35 of 51, above its 34 members. The successor is precommitted (`src/node/finality_tracker.cpp:123-131`) and pinned on the contracts (contracts spec §3.3 step 10), so break or expiry follows and strands both vaults. The stake comes back (no cooldown); honest stakers cannot out-split it without passing 64 | [E] inputs (`src/node/validator_set.h:28-32`; `src/kernel/chainparams.cpp:274-277`; `doc/design/v2-stage0-r1-consensus.md:50-53`); [I] arithmetic |
| 31 extra keys, about 10,300 B3 (full snapshot) | Set above 64: no roots accepted and deposits closed while they stay staked | [I] |
| Above 1/3 by both measures, withholding | Halt; strand at break or expiry | [I] |
| Above 1/3 by both, equivocating | Two conflicting CommitQCs; `reportConflict` halts both verifiers at once (§3.4); strand both vaults; no slashing | [I] |
| At least 2/3 by both | Certify a false root; drain a vault up to its balance | [E] architecture §9 |

Without a bounded committee the headcount half of every row is nearly free.
The contracts spec owner should weigh whether a permanent conflict halt on
both chains at once is the right effect (FD-15).

### 10.3 Bounded committee (FD-14, S5 gate) [I]

Committee(S) = members of snapshot S with weight ≥ W(S)/128, sorted by weight
descending, ties by validator key, truncated to 64. Producers keep the full
snapshot. The committee never exceeds 64; splitting stake below the floor
buys no seats, and keys at the floor are truncated first; the headcount
attack now costs at least (h/2)·W/128, about 13% of weight at h = 34, instead
of 0.005%. The floor is provisional, to be checked against O2's weights.
Alternatives: (b) a weight-only quorum, which makes Sybil keys harmless but
drops the headcount half of the safety bound, against Stage 0's rejection of
smaller thresholds (`b3-flowmesh-v2-stage0.md:710`); (c) a verifier V2 path
letting the outgoing set certify a replacement successor (the incoming
committee minus members silent for a window) when the pinned one has not
certified within it. **S5 gate:** a committee rule ruled before G1 (§7), and
at R_f − 1 a committee of at least 4 with a dual quorum shown online.

### 10.4 Signer liveness

Bridge solvency depends on a dual quorum of the committee running finality
signers, yet nothing rewards signing or penalises absence: the modern reward
is a subsidy for producer and treasury only
(`src/consensus/modern_pos_params.h:55-61`, `:112-120`;
`src/kernel/chainparams.cpp:286-290`), and H1 detaches production rewards
from finality entirely; V1 deadlocked twice (§1).

- **Measure.** O2 gives the online fraction; the S5 gate needs a dual quorum
  online with headroom through a rehearsal.
- **Incentive (FD-16).** Owner ruling before S5. Possible forms: a share of
  the existing subsidy for keys in included certificate bitmaps, or
  eligibility tied to recent participation. No amount is proposed
  (`doc/design/b3-master-handoff.md:1160`).
- **Monitoring (owner-run) [I].** Warn at finalized lag 30 blocks, page at
  90 (burns close); track per-key participation over 100 checkpoints and
  online headcount and weight against both quorums.
- **Runbook [I].** The owner keeps contacts for every committee operator:
  lag 90, contact silent operators; deposits closed (about a day), public
  notice; day 21 after the last rotation (burn gates closed), escalate to all
  stakers; day ~29, earliest possible verifier expiry.

---

## 11. Bootstrapping the new verifiers (finality side)

Contracts spec §5 owns the mechanics. Its default, **START_CERTIFICATE**
(B-11), starts a verifier from an ordinary certificate of a recent epoch k;
there is no genesis statement (its §2.2). B3 supplies [I]:

| Input | Source |
|---|---|
| `GENESIS_*`, `LINEAGE_ID` | R_f − 1, its hash, setHash(Set′_0) and the §6.2 lineage id |
| `START_EPOCH` k, `START_SET_HASH` | Default the newest epoch; its committee's set hash (`getfinalitylineage`, §6.5; `getfinalityset` is V1-only) |
| Start certificate | Any ordinary epoch-k certificate; on mainnet P3-decided, so on B3's committed branch (§9.3) |
| `leaf0Path` | Places historyLeaf(R_f − 1, its hash) at index 0 under that certificate's history root (§6.5) |

At S7 (after S5) the deployer pins these on both chains and anyone submits
one start certificate to both before `INIT_DEADLINE`; a newest k leaves at
most one rotation to catch up. Through 7 days of review deposits stay
closed, anyone checks the pins with `getfinalitylineage` and `getblockhash`,
and watchers can only cancel.

| | START_CERTIFICATE (default) | K_OF_N fallback (`contracts/src/B3FinalityVerifier.sol:93-190`) |
|---|---|---|
| Proves | The pinned keys are live and reach a dual quorum over a history containing leaf 0; not that they are B3's real committee (a deployer pinning a fake set can sign with it) | Three of four attester keys vouch for the pin, perhaps wrongly |
| Extra trusted party | None | Four attesters and their key custody until the deadline |
| Real trust root | Deployer pins, public review against B3, cancel-only watchers | The same, plus the attesters |

It removes a party and needs no walk from epoch 0, so it is the proposed
default (contracts spec C-2); it **departs from the owner-accepted k-of-n
default (architecture §3.1) and needs an owner ruling**. Governing text still says 3-of-4
(`doc/design/b3-master-handoff.md:106`; `doc/design/b3-architecture-contract.md:609`): §16.

---

## 12. Latency and windows

Architecture §7 owns every minute figure (healthy finality, prompt relays,
computed ~78 s blocks, unmeasured until O8); this document uses t_fin =
22-32 blocks (29-42 min).

**Epoch-start wait [I].** An epoch (24-31 h) exceeds the 6 h rotation
minimum (contracts spec §4), and the new committee's first certificate both
rotates the verifier and covers new burns, so normally nothing extra.
Arbitrum's clock error (up to 24 h 13 min) can hold its rotation until about
30 h 13 min after the previous one, at most ~6 h late at the 60 s floor [I
computed]; after missed relays a verifier catches up about 3.2 epochs a day
at 78 s (3 at 60 s).

| Window | Value |
|---|---|
| B3 epoch | 1,440 blocks: 24 h at 60 s, ~31 h at 78 s |
| Contract minimum per rotation | 6 h (21,600 s; contracts spec §4), not V1's 1 day (`contracts/src/IB3FinalityProver.sol:15`) |
| Deposit close | ~24 h after the last relayed certificate, or 72 h after the last rotation, whichever comes first (contracts spec §3.5) |
| B3 lineage break (FD-7 default) | ≥ 47,520 blocks after the epoch start: ≥ 33 days at 60 s, ~43 at 78 s |
| Contract expiry | 30 days after the last rotation; floor 24 days (2,073,600 s; contracts spec §4, B-10) |

**`MAX_CERTIFICATE_AGE`** is 86,400 s on both chains, with relayer
re-certification (contracts spec §4, C-14; architecture B-10); the ~48 h
Arbitrum alternative would split B-10 per chain.

---

## 13. Falsification experiment (model check; not run)

**H-H1:** from R_h every path returns the same producer set; rotation every
E blocks with or without certificates; an escape restarts the schedule.
**H-F:** with P3, the fresh lineage, digest V2 and both verifiers, whenever
faulty keys are below 1/3 **by headcount or by weight**: S1 every accepted
certificate on either verifier lies on one B3 branch; S2 that branch is B3's
committed branch; S3 released ids match canonical leaves; S4 no V1 or
other-lineage object is accepted. Any two dual quorums share more than n/3
keys and more than W/3 weight, so either bound leaves an honest key in the
intersection; Stage 0 states the stronger "and" assumption
(`doc/design/v2-stage0-r1-consensus.md:57-58`). Liveness (L1 certificates
every epoch after stabilization, L2 producers rotate, L3 accepted leaves
stay payable) needs honest members at or above both quorums.

**Model.** n = 4, f = 1, equal weights, then {4,1,1,1} (W = 7, q_w = 5);
checkpoints every 2 heights; depths 2 and 3; reorg limit 8; three epochs; H1
with escape; two verifiers with independent relays, ancestry, root history,
windows and expiry. Faults: one Byzantine key, amnesia (counted faulty),
crashes at every persist, sign and publish point, message loss and
reordering, healing partitions, selective relayers.

| Control | Setup | Expected |
|---|---|---|
| PC1 | V1 voting, 2 of 4 sign A, B overtakes (811,631) | L1 violated |
| PC2 | V1 voting, one orphan lock plus one crash (814,191) | L1 violated with one fault |
| PC3 | Staging + digest V2, 3 of 4 sign A, B overtakes, A relayed first | S2 violated: the verifier is stuck on the orphan root and halts only if a later conflicting certificate is reported; B3 deadlocks |
| PC4 | V1 tracker, no H1, no certificate for E + extension | L2 violated |
| PC5 | Accepted-root history without ancestry | S3 violated |
| NC1 | P3 with f = 2 of 4 | S1 or S2 violated |
| NC2 | P3 weight-only, heavy key Byzantine | Violation; dual rule safe but stalled |
| NC3 | P3, full-snapshot committee, 2 withholding Sybil keys (n = 6, q_h = 5) | L1 violated; with the bounded committee L1 holds |

H-F is falsified by any S-violation within the bound, or an L-violation after
stabilization with a dual quorum online; a run whose positive controls find
nothing is void. Bounds: 12 heights, 2 reorgs, 3 views, 3 epochs.
Tooling: extend `test/flowmesh_v2_agreement/` (equal-weight only,
`PROTOCOL.md:24-31`). A pass is evidence about the specification, not the C++.

---

## 14. Phases, effort and critical path

| Stage | Finality content | Regtest milestone | Mainnet gate |
|---|---|---|---|
| S0 | O2, O8; commit the direction | — | — |
| **S1** | H1 with escape; H2 checkpoint | **RT1** (§5.5) | Review; overrides; R_h ruling; operator coordination |
| S2 | Harness | RT0, RT2, RT3 (V1 digest) | — |
| **S3** | Fresh lineage, digest V2, accumulator, journals, committee rule, `getfinalitylineage`, staging signer, beside the node core V2 slice (node changes §12.1) | **RT4** (§8) | None until S5 |
| S4 | — (Vault_A, forwarders) | RT5-RT7 on the staging lineage | With S7 |
| **S5** | P3; fresh lineage at R_f on mainnet | **RT8**: digest and contracts unchanged; PC3 no longer reproducible | D1, D3, D5, D10, FD-7, FD-12 (C8 split), FD-14, FD-16 ruled; model check; liveness rehearsal; multi-host soak (RP-10); hard fork |
| S6 | Drills | **RT9**: stall past the break, expiry, conflict report, Sybil withholding, reorg undo | Audit; drills green |
| S7 | Deploy both chains; bootstrap from a recent certified checkpoint (k-of-n fallback) | Rehearsed on anvil (regtest plan RT10) | Independent builds agree |

**Components** (C++ lines excluding tests, [H]):

| Component | Basis | Size [H] |
|---|---|---|
| H1 schedule, escape, shared helper, RPC | Adapt the `SetInForceAt` users (§5.2) | 0.5-1.5k |
| Fresh-lineage tracker | Adapt the V1 tracker (`src/node/finality_tracker.cpp`, 363 lines) | 0.8-1.5k |
| History accumulator | Adapt the withdrawal tree (`src/modern/withdrawal_tree.h`, 166 lines) | 0.3-0.6k |
| Digest V2 types, carrier, vote message, journal namespace | Adapt the V1 finality code (about 2,600 lines in types, certificate, signature, signer store, transport) | 1.5-3k |
| Committee rule, `getfinalitylineage`, staging gate | New | 0.5-1k |
| P3 engine | Adapt FlowMesh's PBFT engine (`src/flowmesh/agreement_wire.h:24-26`; `src/node/flowmesh_agreement.cpp`, 991 lines), which counts equal-weight seats (`src/flowmesh/preagreement.cpp:93`, `:115`): add dual thresholds, rosters, checkpoint values, carrier binding | 3-6k |
| Model check | Extend the equal-weight Python model | 1-2k Python |

**Critical path:** rulings (D1, D3, D5, D10, FD-7, FD-12, FD-14, FD-16) →
model check → P3 → RT8 → audit → hard fork → S7 deploy → 7-day review → S8.
Order of magnitude [H]: H1 to a green RT1 in 2-4 person-weeks; the staging
path 23-39 person-weeks to RT7 (25-43 to RT9), excluding P3 and audit
(regtest plan §8); P3 to mainnet in roughly 3-9 months, dominated by
rulings, the model check, audit and rollout, which FD-10's interim lineage
would not shorten.

---

## 15. Owner decisions (FD-n → B-n)

| ID | Decision | Default | Maps to |
|---|---|---|---|
| FD-1 | H1 as its own hard fork, before any mainnet fresh lineage | Yes, with the §5.4 overrides | B-6 |
| FD-2 | H1 schedule, carry-over, fallback, escape at 2,880 rounds | Yes | B-6 |
| FD-3 | H2 hardened checkpoint in the H1 release | Mandatory | B-6 |
| FD-4 | R_h, adoption window, late-upgrader notice | Owner, after O2 | B-6 |
| FD-5 | Genesis rule (§6.2) | Yes | B-8 |
| FD-6 | Reuse bindings; journals per (lineage id, key) | Yes | B-8 |
| FD-7 | Fresh-lineage break deadline | Extension ≥ 46,080 (as B-23); or no B3-side break | B-23 |
| FD-8 | A break fails closed; a re-bootstrap needs new contracts and strands the old vaults | Yes, disclosed | B-8 |
| FD-9 | Digest V2 field sources (§7) | Yes; freeze at G1 | B-9 |
| FD-10 | No mainnet interim V1-voting lineage; staging regtest-only | Yes | B-7 |
| FD-11 | Bootstrap from a recent certified checkpoint (START_CERTIFICATE, P3-decided on mainnet); k-of-n fallback | Yes, pending a ruling (departs from the owner-accepted k-of-n default); reconcile governing text | B-11 |
| FD-12 | Stage 0 amendments (§9.2), including the C8 split and F := G := R_f − 1 | Yes, before S5 | B-20, V2-D10 |
| FD-13 | V2-D1/D3/D4 defaults (§9.4) | As listed | S5 gate |
| FD-14 | Committee rule: bounded, weight-only, or replacement successor | Bounded, floor W/128, cap 64 | B-22, V2-D5 |
| FD-15 | Contracts: permanent conflict halt on both chains; replacement-successor and succession paths | Raise with the contracts spec owner | B-9 |
| FD-16 | Signer participation incentive | Owner ruling before S5; no default amount | V2-D4 |

---

## 16. Governing text to reconcile (finality items)

Each needs a reviewed commit first; architecture §15 carries every row (its
item numbers in brackets).

| Text | Needed |
|---|---|
| `CLAUDE.md:87-88` (rule 3); `CLAUDE.md:12-13` [2] | One override: H1 with its escape, H2, the fresh lineage and the B-22 committee, together with the bridge V2 consensus regime (node changes §3-§10) |
| `doc/design/b3-cross-chain-finality-v1.md:3-11`, `:172-178` [10] | V2 objects beside a frozen V1; H1 breaks the one-stake-universe identity and handover-gated production; the bounded committee changes the checkpoint set function; row 175's re-bootstrap specified as §6; journal per lineage |
| `doc/design/b3-master-handoff.md:106`; `doc/design/b3-architecture-contract.md:609` [6] | Bootstrap from a recent certified checkpoint; k-of-n fallback |
| Proposals to amend: `b3-flowmesh-v2-stage0.md:43`, `:69`, `:709`, `:711` [8]; `v2-stage0-r1-consensus.md:370-376`, `:404-480` (incl. `:407`), `:466-467` [9] | §9.2 |

---

## 17. Open items and owner reads

- **O2** lineage state, Set_3 size, weights (for the W/128 floor), online and
  unstaked members; **O8** block spacing.
- **FD-15** (contracts spec §3.4, §14): whether a conflict halt should stop
  both verifiers for good, and a replacement-successor or succession path.
- **Owner rulings:** B-11's departure from the accepted k-of-n default (FD-11), FD-7, FD-12, FD-14,
  FD-16; a pilot reserve ceiling may be wanted given §10.1 (B-14 has none).
- Unmeasured [H]: PBFT round time; verifier V2 gas with the ancestry path;
  accumulator cost; escape fork-choice behaviour.
- Not yet run: the model check; RT1, RT4, RT8, RT9.
- Social: Set_3 cooperation for H1; committee liveness.

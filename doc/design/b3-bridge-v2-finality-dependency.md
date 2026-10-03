# B3 Bridge V2: finality dependency, H1 and the fresh lineage

> **Status: PROPOSAL, no governing authority.** Below every owner ruling and
> every reviewed, committed governing document; nothing ratified, built,
> model-checked, audited or activated.
>
> - 2026-10-04, revision 10 (the **simpler plan**), base `1d1d6da`, branch
>   `feat/flowmeshV2-dev-claude`. Nothing run; no node, chain RPC or signer
>   contacted. Follows the owner's 2026-10-03 simpler-plan ruling and the
>   adopted Codex review, as relayed in
>   [b3-bridge-v2-arbitrum-proposal.md](b3-bridge-v2-arbitrum-proposal.md)
>   (the "architecture document").
> - Since revision 9: Arbitrum and LP references removed; B-37's per-epoch
>   keys replace FD-6's binding reuse (§6.4); K_OF_N bootstrap (§11).

**Labels.** [E] established or verified; [I] design choice; [H] estimate
or unverified. **Default**: a provisional solve-and-report choice, open to
owner ruling. Repo facts are `path:line` at `1d1d6da`; `.../study/` is
`/Users/josh/development/ON/B3-FlowMesh-200ms-evidence/arbitrum-bridge-study-20261003/`.

**Scope.** This document owns B3's finality repair (§4), **H1** (§5), the
**fresh lineage and re-bootstrap** (§6), B3's source for each digest V2 value
(§7), the regtest staging signer (§8), **V2 P3** (§9), what a lineage failure
costs the bridge (§10), the finality side of bootstrap (§11) and the model
check (§13). Byte layouts are the contracts spec's (AssetId: node changes §3.2). Ids
follow the architecture document: **B-n** (its §15), **S0-S9** and
**RT0-RT10** (its §14), **O-n** (its §19); its §8 owns every minute figure.
Sub-decisions here are **FD-n** (§15).

---

## 0. Short version

1. **B3 needs H1 for itself.** Under V1 producers rotate only after a
   finality certificate. Mainnet's lineage is expected broken since about
   826,841, so producers are frozen at Set_3 for good [H; O2]: new stakers
   never produce, an unstaked member produces forever, and no Set_3 key
   online halts the chain. H1 is a height-gated hard fork rotating producers
   every 1,440 blocks, with a 24-hour **escape** so an offline producer set
   cannot halt the chain; no BLS, bridge or Ethereum code. Expedite it if O2
   shows few Set_3 keys producing (S1, B-6).
2. **Reorg protection.** A fresh node is protected only to the hardened
   checkpoint 811,641 until it validates the block carrying the 814,051
   certificate, so a hardened checkpoint (**H2**) is mandatory in the H1
   release.
3. **The bridge does not drive H1 but is ordered after it, with no
   deadline.** The old verifier lapses 2026-10-08T17:11:59Z, owed nothing
   (B-1, B-2). On mainnet the bridge opens only after V2 P3
   exports digest V2 (B-7).
4. **Fresh lineage (B-8).** A re-bootstrap at R_f with its genesis committee
   from Snapshot(R_f − 1), a new lineage id, tags and journal namespace, so
   old orphan locks cannot block it and no old signature validates under it
   (anticipated at `doc/design/b3-cross-chain-finality-v1.md:175`). Members
   use fresh per-epoch keys (B-37, §6.4).
5. **Digest V2 (B-9)** binds the lineage id and a checkpoint-history root, so
   the verifier rejects a certificate not extending its last accepted one.
   Mainnet still needs an exporter that never exports a checkpoint B3 can
   abandon, nor two conflicting quorum certificates (V1 voting exported
   811,631's orphan, `doc/release-notes-v1.1.3.md:100-106`): P3's job.
6. **A lineage failure strands the reserve.** The single Ethereum vault is
   bound for ever to one verifier and one lineage. A B3-side break, a 30-day
   expiry, a conflict halt or a permanent quorum loss locks every token not
   under an accepted root, up to **100% of the backing**, beyond a
   re-bootstrap's reach. Hence: a break deadline later than expiry (FD-7,
   B-23); a bounded committee against a headcount Sybil costing about 17
   keys of 333 B3 today (FD-14, B-22, **needs explicit owner approval**);
   and signer liveness (§10.4).
7. **Regtest first.** Before P3 the V1 single-vote signer signs digest V2
   behind a regtest-only gate (RT4); P3 then replaces only the voting (RT8).

---

## 1. V1 finality and why it stalled

All [E].

- **Vote.** One BLS vote per member per checkpoint, `(h − M) mod 10 = 0`
  (`src/modern/finality_schedule.h:61-64`), over a 112-byte object tagged
  `B3/FINALITY/V1` (`src/modern/finality_types.h:64-107`, `:294-300`).
  Consensus depth 12; wallet export waits 20
  (`src/kernel/chainparams.cpp:293-294`; `src/node/finality_signing_policy.h:16`).
- **Journal.** Per (chain domain, key); the lock moves only on a newer
  included certificate of the same epoch and set, or a compiled pin
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
(`src/node/finality_tracker.h:79-84`).

**Incidents:** 811,631, pinned to 811,641
(`doc/release-notes-v1.1.3.md:57-106`); 814,191, 21 of 23 signers
(`doc/finality-quorum-deadlock-814191.md:16-20`); the expected break near
826,841 [H].

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
  relaying 814,051: default skip unless O9 finds a burn leaf at or below it
  (B-2).
- **B3.** Nothing is deleted. The V1 tracker keeps computing the old state
  for history and the legacy burn rule, which stays closed
  (`src/node/finality_tracker.cpp:46-63`; B-19); V1 certificates stay
  invalid while the flag is set (`src/modern/finality_schedule.h:141`).
- **Why no V1 signature verifies under V2** [I]: the signed message differs
  in tag, length and lineage id (contracts spec §2.2), and under B-37 every
  fresh-lineage key is new (§6.4), so no V1 key signs for V2. Separately,
  ruleset-2 set headers stop a V1 header from bootstrapping V2
  (`contracts/src/IB3FinalityProver.sol:13`, `:64`).

---

## 4. Chain health: does B3 need a repair now?

**Yes.** With the lineage broken, V1's producer set never changes again:

| Area | Effect while frozen | Label |
|---|---|---|
| Blocks | Continue while one Set_3 key is online (~240 s blocks for a 1/34 key once eligibility saturates). No Set_3 key online: halt, permanent if those keys never return | [E] `src/modern/pos_v1.h:74-79`, `:109-123`; `src/validation.cpp:2574-2577` |
| New stakers | Never produce or earn | [E] `src/node/staking.cpp:669`; `src/test/finality_eligibility_tests.cpp:75-82` |
| Unstaking | Ordinary spend, no cooldown; a member can withdraw all stake, sell its key and keep producing | [E] `doc/design/b3-modern-pos-spec.md:23-24` |
| Reorg protection | Online nodes: 1,440 blocks below their tip, not during import (`src/validation.cpp:6655-6666`). Fresh node: hardened 811,641 only, until it validates the 814,051 certificate's block (`:2511-2535`) | [E] |
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

The regtest plan owns the harness, knobs and fixture (its §3.6). Its RT1
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

It needs no Set_3, orphan-lock pins or deadline, and the new contracts can
demand ancestry and a lineage id.

### 6.2 Genesis rule [I]

| Item | Default |
|---|---|
| Activation | Height-gated R_f in a reviewed hard fork (owner-set; regtest first) |
| Grid | V1's, `(h − M) mod checkpoint_interval = 0` (contracts spec §2.3); R_f − 1 is on it (mainnet: ends in 1), is leaf 0 and the contracts' `GENESIS_HEIGHT` |
| Genesis committee | Set′_0 = Set′_1 = Committee(Snapshot(R_f − 1)) with ruleset-2 headers (Committee: §10.3; keys: §6.4) |
| Authority | The release itself, as V1's bootstrap at M (`src/node/finality_tracker.cpp:107-120`; `doc/design/b3-cross-chain-finality-v1.md:175`). No old-set vote |
| Lineage id | Contracts spec §2.2 with genesisHeight = R_f − 1, that block's hash and setHash(Set′_0) |
| Epochs, producers | Epochs restart at 0. Producers are unchanged by R_f (full snapshots, H1 schedule); the committee has its own epoch boundaries |
| Chain domain | Unchanged; regtest's differs [E] (`src/modern/chain_domain.h:24`) |

### 6.3 Parameters

E = 1,440, interval 10, depth 12, `min_finality_set` 2
(`src/kernel/chainparams.cpp:291-300`); export wait 20
(`src/node/finality_signing_policy.h:16`); bridge qualification 4-64 members
and weight ≥ 900 (contracts spec §3.1; `src/kernel/chainparams.cpp:263-265`),
B-22 selecting the committee (§10.3). Four is the smallest set that
tolerates one Byzantine member; a 2-of-3 set tolerates none (architecture
§18, Codex review).

**Break deadline (FD-7), default changed.** V1's extension of 10,080 would
break the fresh lineage 11,520 blocks into a stall (8 days at 60 s), while
the verifier accepts the same committees for 30 days and a break strands
the reserve (§10.1). Default, as architecture §9.1 (B-23): **extension
≥ 46,080**, so E + extension ≥ 47,520 blocks: at least 33 days at the 60 s
floor (`src/validation.cpp:6646-6654`), 30-day expiry plus 3 days of relay
margin; about 43 at 78 s. A quorum recovering while the verifier lives
revives the same contracts. Cost: a stalled committee keeps checkpoint
authority 33-43 days; producers are unaffected (H1); burns close within
about 2 h (node changes §8.4, C3). Alternative: no B3-side break. Either is
a new constant for the owner (`doc/design/b3-master-handoff.md:1160`).

### 6.4 Keys and journals

- **Per-epoch keys (B-37, Codex review).** Today a finality BLS key is
  derived from the identity secret (`src/wallet/wallet.cpp:3937-3949`) or is
  one imported key in the wallet file, preferred on a fresh bind
  (`src/wallet/wallet.h:1051-1067`; `src/wallet/wallet.cpp:4030-4031`);
  either way an old backup yields it. Default [I], as architecture §6: each
  member signs with a freshly generated key, kept outside the wallet file
  (node changes §7.7) and used only while its committee is seated.
- **Own binding kind [I].** Fresh keys use a new binding record, not V1's
  FINALITY_KEY. V1's one binding per validator feeds the V1 tracker and
  producer snapshots (`src/node/finality_binding_index.h:24-45`), and the V1
  signer signs only with a wallet key matching it
  (`src/wallet/wallet.cpp:4016-4028`); rebinding it each epoch would stop V1
  certifying and break an intact V1 lineage (regtest fixture I, whose
  production then pauses, `src/node/miner.cpp:558-560`; mainnet if O2 finds
  it intact). V1 bindings, signing and producer eligibility stay unchanged.
- **Erasure and its limit.** A key is erased once the handover out of its
  last seated epoch is certified on B3 and 1,440 blocks deep (the reorg
  limit, `src/validation.cpp:6655-6666`), with no verifier condition: a
  lagging verifier catches up with certificates already on B3, each
  transition signed by the incoming set (contracts spec §3.3 steps 2, 7), so
  waiting would only lengthen the time stolen keys could forge a root. This
  protects past epochs only: bindings are authorized by the identity key's
  BIP340 signature (`src/modern/finality_key.h:32-34`), held in every backup
  (`src/wallet/wallet.h:1037-1038`), so an old backup can bind a key for a
  coming epoch. Default [I]: **two fresh bindings by one member for one
  epoch void its seat for that epoch**, and wallets and monitors alert; a
  backup holder can deny a seat, and take one only when the member's signer
  does not bind. That key already signs the member's blocks
  (`src/wallet/wallet.h:1024-1031`), so a leak calls for restaking under a
  new identity. Rejected: co-signing bindings with the seated key, which
  leaves a member that sat out or lost its signer store only the identity
  key.
- **Rebinding schedule** [E today's rules; I schedule; open]. Bindings take
  effect only at the next snapshot (`src/node/finality_binding_index.h:42-45`);
  today a validator may re-bind its own key (`src/modern/finality_key.h:42-44`),
  so "never bound twice" is new. Proposed: (1) fresh bindings start at a
  ruled height R_key below R_f − 1 and refuse any key ever bound under either
  kind; (2) a fresh binding names its epoch k+1 and counts only if included
  after Snapshot(start_(k−1) − 1) and by Snapshot(start_k − 1), which fixes
  Set′_(k+1); Committee() seats only holders, so no V1 key enters; (3)
  Set′_0 = Set′_1 share one genesis binding made between R_key and R_f − 1;
  (4) **carry-over**: below `min_finality_set` members the committee in force
  and its keys serve another epoch (`src/node/finality_tracker.cpp:72-83`)
  without rebinding; bindings for the skipped epoch go unused. Erasure is
  signer policy (the chain sees only public keys,
  `src/wallet/wallet.h:1047-1048`); the contracts do not change (contracts
  spec §3.7).
- **Journals.** A new store keyed by (lineage id, key). Old journals are
  never deleted, reset or read for new-lineage signing; no V1 vote combines
  with V2 votes into anything accepted, so old locks protect only the dead
  lineage (a key locked on the 814,191 orphan would otherwise refuse every
  new branch, `src/node/finality_signature.cpp:607-618`).

### 6.5 Tracker, carrier, transport and RPC [I]

- **Tracker.** Separate from producers; V1's handover-gated epochs under
  staging, P3-R1's under P3 (`doc/design/v2-stage0-r1-consensus.md:334-350`).
- **Breaks (FD-8).** Not healed automatically. A ruled re-bootstrap makes a
  new lineage and needs new contracts; it **cannot release anything held by
  the existing vault** (§10.1), so it is a stranding event, not a recovery,
  unless verifier V2 gains a succession path the live set signs before
  expiry (FD-15).
- **Carrier.** A new coinbase record carries FinalizedBlockV2, bitmap and
  aggregate signature, judged like V1 (`src/modern/finality_schedule.h:132-172`)
  plus lineage id, index, history root and ruleset 2; under P3 it binds the
  CommitQC (`doc/design/b3-flowmesh-v2-stage0.md:592-597`).
- **Votes.** Staging votes need a lineage-naming message (V1 `finsig` has
  none, `src/net_processing.cpp:6636-6702`); under P3, Stage 0's PBFT
  messages.
- **RPC.** New `getfinalitylineage`: lineage id, R_f − 1 and its hash,
  committee headers and set hashes by epoch, history frontier, history paths
  (ancestry, `leaf0Path`, §11) and status. `getfinalityset` serves only V1
  sets (`src/rpc/blockchain.cpp:3707-3716`).

### 6.6 Where the lineage starts

Regtest: S3 under the staging signer (RT4). Mainnet: S5 with P3 (B-7).
**Considered, not default (FD-10):** a mainnet interim lineage under V1
voting for B3's own pin. It would bring back V1's deadlocks and orphan
certificates; if wanted, it must use a lineage id that no bridge contract
ever accepts.

---

## 7. Digest V2: where B3 gets each value

Contracts spec §2.1-§2.3 defines every byte; this document restates none.
B3's sources [I]:

| Field | B3 value |
|---|---|
| lineageId | Fixed at R_f (§6.2) |
| height, blockHash | The grid checkpoint on the validating chain |
| withdrawalRoot | **Zero below V** (`BRIDGE_ACTIVATION_HEIGHT`), then `FinalityWithdrawalRootV2(height)`, non-zero even when empty, as V1 (`src/node/bridge_state.cpp:1162-1171`; contracts spec §2.4); burns open at W2 = V |
| validatorSetHash | keccak of the successor's ruleset-2 SetHeader |
| epoch | Fresh-lineage epoch of the signing committee |
| checkpointIndex | (height − (R_f − 1)) / checkpoint_interval (10 mainnet, 5 regtest), below 2^32 |
| checkpointHistoryRoot | Root over leaves i = 0..k−1, leaf i = historyLeaf((R_f − 1) + i·interval, blockHash_i) |
| bridgeFlags | Bit 0 once a sticky conflict at or below height covers an entry of this verifier (node changes §7.4, §8.5); the verifier then closes deposits |

A history root lets relayers skip certificates (contracts spec §2.3). Every
node keeps the 32-hash frontier per chain position, with reorg undo and
reindex rebuild, reusing the withdrawal tree
(`src/modern/withdrawal_tree.h:33-34`); consensus rejects a V2 carrier whose
index, root or flags differ, as V1 does for the withdrawal root
(`src/modern/finality_schedule.h:157-167`). **Shared
C++/Foundry vectors** for lineageId, historyLeaf, the root at k, a leaf-0
path and digestV2 go into RT4 and Stage 0's G1 freeze
(`doc/design/b3-flowmesh-v2-stage0.md:933-934`).

**What ancestry fixes, and what it does not.** It rejects a certificate that
does not extend the last accepted checkpoint (RC4). It cannot pick the canonical branch: an orphan certificate that extends the
last accepted root and arrives first is accepted, then the canonical chain
is rejected, and a later conflicting certificate can halt the verifier
(contracts spec §3.4). Mainnet therefore requires **no exported certificate
for a checkpoint B3 fork choice can abandon, and no two conflicting quorum
certificates.** V1 voting fails the first (811,631), so mainnet waits for
P3 (B-7).

---

## 8. Regtest staging signer

The V1 single-vote signer, unchanged in logic, signs digest V2 under the
fresh lineage with its own journal namespace and the V2 carrier, in a
"staging" mode that exists only in regtest params [I]. It makes the bridge
testable end to end before P3 but proves no BFT safety: a reorg past a
signed checkpoint yields an orphan certificate plus a deadlock (RC1-RC3),
and conflicting quorum certificates arise across an epoch boundary or with
amnesia; the regtest plan's §3.2 certificate tool makes them
(`src/bridge/regtest_certify.cpp`, planned).

**RT4 (S3), finality items** (in the regtest plan's RT4, except item 6,
which is its §6.2 RT9 drill on fixture B):

1. Fresh lineage at R_f = 262, R_f − 1 = 261 on the 131 + 5k grid (regtest
   plan §3.6): fixture I first (V1 intact, no H1; H1 orders only the mainnet
   lineage, §4), then fixture B (H1 at 220). The genesis committee equals
   Committee(Snapshot(R_f − 1)).
2. A member whose V1 key holds an old-journal orphan lock signs in the new
   lineage.
3. B3's V2 carrier rule rejects a V1-tagged signature, a ruleset-1 header
   and an other-lineage certificate.
4. The anvil verifier starts from a 3-of-4 attestation of a staging
   statement (epoch 0, and k ≥ 2) plus `leaf0Path` (§11), and the
   start-certificate alternative initializes; `getfinalitylineage` matches
   the pins; shared vectors pass.
5. Routes (a) and (d1) round trip; ancestry rejection; a release survives a
   newer root.
6. Stalled lineage: deposits close after 24 h without certificates, 72 h
   with keep-alives but no rotation; producers keep rotating (its RT9 row 1).
7. Per-epoch keys (B-37): a key bound before, under either kind, is
   refused; V1 keeps certifying on fixture I throughout; a member without a
   fresh binding sits out, and a second binding by an old-backup copy voids
   the seat; epochs 0 and 1 share keys, epoch 2 does not; a carried-over
   committee keeps its keys until its handover is past the reorg limit.

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
`doc/design/v2-stage0-r1-consensus.md:3`).

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
| Timely certificates | Export follows each decision | Live quorum; timeouts unselected [H] |
| No exported checkpoint B3 can abandon | Export only after a durable CommitQC, and the committed checkpoint bounds fork choice (`b3-flowmesh-v2-stage0.md:576-581`, `:599-604`) | Holds while safety holds |
| No conflicting quorum certificates | Quorum intersection | Faulty below 1/3 by headcount **or** by weight (§13) |
| Epoch continuity | Next epoch needs a carrier with the prior CommitQC (`v2-stage0-r1-consensus.md:352-357`) | A missing export stalls the next epoch |
| Committee of 4-64, Sybil-resistant | Not addressed | FD-14 (S5 gate; B-22 needs explicit owner approval) |
| Producers never freeze | Only with the H1 amendment | — |

### 9.4 Latency and decisions

PREPARE and COMMIT fit in the 8 blocks (about 10 min) between depth 12 and
the 20-block export wait, leaving t_fin unchanged; PBFT round time is
unmeasured [H].

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

The vault's `VERIFIER` is immutable (contracts spec §6.1); the verifier
accepts only its immutable `LINEAGE_ID`, from the current or next set (§3.1,
§3.3 step 4); funds leave only by release of a certified leaf (its §10).
Releases against accepted roots continue after expiry, halt or cancel;
everything else, the backing of all bUSD V2 not under an accepted root, can
never leave, and there is no other reserve (architecture §9.1). Triggers:
a fresh-lineage break (FD-7); the verifier's 30-day expiry; a conflict halt
(`reportConflict`, contracts spec §3.4), even after the canonical
certificate landed first; permanent quorum loss, including the Sybil halt
(§10.2).

Burns close first (node changes §8.4), so holders cannot exit through B3
either. B-23's migration leaf moves the reserve to a pinned
successor only while the verifier lives; the residual needs explicit owner
acceptance (architecture §9.1, §13); regtest plan §6.2 drills each.

### 10.2 Who can do what

| Faulty share | Can do | Label |
|---|---|---|
| About n/2 extra keys at 333 B3 each, withholding (full-snapshot committee) | Halt for good: 17 keys (5,661 weight, about 0.005% of Set_2's 126,108,063) raise Set_2's headcount quorum to 35 of 51, above its 34 members. The successor is precommitted (`src/node/finality_tracker.cpp:123-131`) and pinned on the contract (contracts spec §3.3 step 10), so the vault strands at break or expiry. The stake comes back (no cooldown) | [E] inputs (`src/node/validator_set.h:28-32`; `src/kernel/chainparams.cpp:274-277`; `doc/design/v2-stage0-r1-consensus.md:50-53`); [I] arithmetic |
| 31 extra keys, about 10,300 B3 (full snapshot) | Set above 64: no roots, deposits closed while staked | [I] |
| Above 1/3 by both measures, withholding | Halt; strand at break or expiry | [I] |
| Above 1/3 by both, equivocating | Two conflicting CommitQCs; `reportConflict` halts the verifier (contracts spec §3.4) and the vault strands | [I] |
| At least 2/3 by both | Certify a false root; drain the vault up to its balance | [E] architecture §10 |

Without a bounded committee the headcount half of every row is nearly free.
Whether a conflict halt should be permanent is FD-15; kind 13 and
`reportConflict` take only current or next sets (node changes §8.5).
Slashing would not make these rows dollar protection (architecture §10).

### 10.3 Bounded committee (FD-14, S5 gate) [I]

Committee(S) = members of snapshot S with weight ≥ W(S)/128, sorted by weight
descending, ties by validator key, truncated to 64. Producers keep the full
snapshot. The committee never exceeds 64; splitting stake below the floor
buys no seats, and keys at the floor are truncated first; the headcount
attack now costs at least (h/2)·W/128, about 13% of weight at h = 34, instead
of 0.005%. The floor is provisional until O2.
**B-22 needs explicit owner approval** (Codex review;
`doc/design/b3-flowmesh-v2-stage0.md:711`). Alternatives: (b) a weight-only
quorum, harmless to Sybil keys but dropping the headcount half of the
safety bound, which Stage 0 rejects (`b3-flowmesh-v2-stage0.md:710`); (c) a
verifier V2 path letting the outgoing set certify a replacement successor
when the pinned one stays silent. **S5 gate:** a committee rule ruled
before G1 (§7), and at R_f − 1 a committee of at least 4 with a dual quorum
shown online.

### 10.4 Signer liveness

Solvency needs a dual quorum of the committee running finality signers,
yet nothing rewards signing or penalises absence: the modern reward goes to
producer and treasury only
(`src/consensus/modern_pos_params.h:55-61`, `:112-120`;
`src/kernel/chainparams.cpp:286-290`), H1 detaches production from finality,
and V1 deadlocked twice (§1).

- **Measure.** O2 gives the online fraction; the S5 gate needs a dual quorum
  online with headroom through a rehearsal, including B-37 sit-outs.
- **Incentive (FD-16).** Owner ruling before S5, for example a subsidy share
  for keys in included certificate bitmaps; no amount is proposed
  (`doc/design/b3-master-handoff.md:1160`).
- **Monitoring and runbook (owner-run) [I].** Warn at finalized lag 30
  blocks, page at 90 (burns close); track participation, fresh bindings and
  online quorums. Escalate: silent operators at lag 90; public notice when
  deposits close (about a day); all stakers by day 22 after the last
  rotation (burns closed, gate E2); day 30, verifier expiry (§12).

---

## 11. Bootstrapping the new verifier (finality side)

Contracts spec §5 owns the mechanics. Its default, **K_OF_N** (B-11), is a
time-bounded 3-of-4 attestation (k-of-n accepted in conversation on
2026-10-03, uncommitted, conditional on no more trust-minimized option):
four published keys sign one recent epoch-k statement once, before a
deadline. B3 supplies [I]:

| Input | Source |
|---|---|
| `GENESIS_*`, `LINEAGE_ID` | R_f − 1, its hash, setHash(Set′_0) and the §6.2 lineage id |
| `START_EPOCH` k, `START_SET_HASH` | Newest epoch; its committee's set hash (`getfinalitylineage`) |
| Start statement | B3's canonical epoch-k statement; on mainnet P3-decided (§9.3) |
| `leaf0Path` | historyLeaf(R_f − 1, its hash) at index 0 under its history root |

At S7 (after S5) the deployer pins these; anyone checks them with
`getfinalitylineage` and `getblockhash` during the review. Deposits still
wait for an ordinary certificate. The trust root is the four keys, the pins,
public review and cancel-only watchers. **Recommended alternative, needing
an owner ruling:** START_CERTIFICATE, an ordinary epoch-k certificate with
no bootstrap keys; it proves the pinned keys live, not that they are B3's
committee (contracts spec §5). Governing text says 3-of-4 for V1's
Set_0 (`doc/design/b3-master-handoff.md:106`;
`doc/design/b3-architecture-contract.md:609`): §16.

---

## 12. Latency and windows

Architecture §8 owns every minute figure (computed ~78 s blocks,
unmeasured until O8); this document uses t_fin = 22-32 blocks (29-42 min).

**Epoch-start wait [I].** An epoch (24-31 h) exceeds the 6 h rotation
minimum (contracts spec §4); the new committee's first certificate rotates
the verifier and covers new burns.

| Window | Value |
|---|---|
| B3 epoch | 1,440 blocks: 24 h at 60 s, ~31 h at 78 s |
| Contract minimum per rotation | 6 h (contracts spec §4), not V1's 1 day (`contracts/src/IB3FinalityProver.sol:15`) |
| Deposit close | ~24 h after the last certificate or 72 h after the last rotation, whichever is first, or for good on B3's conflict flag (contracts spec §3.5) |
| B3 lineage break (FD-7 default) | ≥ 47,520 blocks after the epoch start: ≥ 33 days at 60 s, ~43 at 78 s |
| Contract expiry | 30 days after the last rotation (contracts spec §4, B-10) |

---

## 13. Falsification experiment (model check; not run)

**H-H1:** from R_h every path returns the same producer set; rotation every
E blocks with or without certificates; an escape restarts the schedule.
**H-F:** with P3, the fresh lineage, digest V2 and verifier V2, whenever
faulty keys are below 1/3 **by headcount or by weight**: S1 every accepted
certificate on the launch verifier, or on a second independently relayed
instance (architecture §17 item 1), lies on one B3 branch; S2 that branch
is B3's committed branch; S3 released ids match canonical leaves; S4 no V1 or
other-lineage object is accepted. Any two dual quorums share more than n/3
keys and more than W/3 weight, so either bound leaves an honest key in the
intersection; Stage 0 states the stronger "and" assumption
(`doc/design/v2-stage0-r1-consensus.md:57-58`). Liveness (L1 certificates
every epoch after stabilization, L2 producers rotate, L3 accepted leaves
stay payable) needs honest members at or above both quorums.

**Model.** n = 4, f = 1, equal weights, then {4,1,1,1} (W = 7, q_w = 5);
checkpoints every 2 heights; depths 2 and 3; reorg limit 8; three epochs; H1
with escape; the launch verifier plus that second instance (a superset of
launch), relayed independently, each with ancestry, root history, windows
and expiry. Faults: one Byzantine key, amnesia (counted faulty), crashes at
every persist, sign and publish point, message loss and reordering, healing
partitions, selective relayers.

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
| S0 | O2, O8; commit the 2026-10-03 rulings | — | — |
| **S1** | H1 with escape; H2 checkpoint | **RT1** (§5.5) | Review; overrides; R_h ruling; operator coordination |
| S2 | Harness | RT0, RT2, RT3 (V1 digest) | — |
| **S3** | Fresh lineage, digest V2, accumulator, journals, per-epoch keys (B-37), committee rule, `getfinalitylineage`, staging signer | **RT4** (§8) | None until S5 |
| S4 | — (forwarders; legacy closure) | RT6 on staging; RT5 retired, RT7 withdrawn for launch | With the S7 pins |
| **S5** | P3; fresh lineage at R_f on mainnet | **RT8**: RT4 and RT6 under P3; PC3 no longer reproducible | D1, D3, D5, D10, FD-7, FD-12, FD-14, FD-16 ruled; model check; liveness rehearsal (with B-37 sit-outs); multi-host soak (RP-10); hard fork |
| S6 | Drills | **RT9**: stall past the break, expiry, conflict report, Sybil withholding, reorg undo | Audit; drills green |
| S7 | Deploy on Ethereum; 3-of-4 bootstrap of a recent epoch | Rehearsed on anvil (RT10) | Independent builds agree |

**Components** (C++ lines excluding tests, [H]):

| Component | Basis | Size [H] |
|---|---|---|
| H1 schedule, escape, helper, RPC | Adapt `SetInForceAt` users (§5.2) | 0.5-1.5k |
| Fresh-lineage tracker | Adapt `src/node/finality_tracker.cpp` (363 lines) | 0.8-1.5k |
| History accumulator | Adapt `src/modern/withdrawal_tree.h` (166 lines) | 0.3-0.6k |
| Digest V2 types, carrier, vote message, journal namespace | Adapt the V1 finality code (about 2,600 lines) | 1.5-3k |
| Committee rule, `getfinalitylineage`, staging gate | New | 0.5-1k |
| Per-epoch keys (B-37) | New and adapted (§6.4) | Not estimated |
| P3 engine | Adapt FlowMesh's PBFT engine (`src/node/flowmesh_agreement.cpp`, 991 lines; equal-weight seats, `src/flowmesh/preagreement.cpp:93`, `:115`): add dual thresholds, rosters, checkpoint values, carrier binding | 3-6k |
| Model check | Extend the equal-weight Python model | 1-2k Python |

**Critical path:** rulings (D1, D3, D5, D10, FD-7, FD-12, FD-14, FD-16) →
model check → P3 → RT8 → audit → hard fork → S7 deploy → 7-day review → S8.
Order of magnitude [H]: H1 to a green RT1 in 2-4 person-weeks; the staging
path not re-estimated (architecture §11); P3 to mainnet roughly 3-9 months,
dominated by rulings, model check, audit and rollout.

---

## 15. Owner decisions (FD-n → B-n)

| ID | Decision | Default | Maps to |
|---|---|---|---|
| FD-1 | H1 as its own hard fork, before any mainnet fresh lineage | Yes, with the §5.4 overrides | B-6 |
| FD-2 | H1 schedule, carry-over, fallback, escape at 2,880 rounds | Yes | B-6 |
| FD-3 | H2 hardened checkpoint in the H1 release | Mandatory | B-6 |
| FD-4 | R_h, adoption window, late-upgrader notice | Owner, after O2 | B-6 |
| FD-5 | Genesis rule (§6.2) | Yes | B-8 |
| FD-6 | Per-epoch keys under their own binding kind, §6.4 schedule, a double binding voiding the seat (replaces binding reuse); journals per (lineage id, key) | Yes; schedule open | B-37, B-8 |
| FD-7 | Fresh-lineage break deadline | Extension ≥ 46,080 (as B-23); or no B3-side break | B-23 |
| FD-8 | A break fails closed; a re-bootstrap needs new contracts and strands the existing vault | Yes, disclosed | B-8 |
| FD-9 | Digest V2 field sources (§7) | Yes; freeze at G1 | B-9 |
| FD-10 | No mainnet interim V1-voting lineage; staging regtest-only | Yes | B-7 |
| FD-11 | Bootstrap of a recent epoch (P3-decided on mainnet) | 3-of-4 attestation (k-of-n accepted in conversation, uncommitted); recommended START_CERTIFICATE alternative needs a ruling | B-11 |
| FD-12 | Stage 0 amendments (§9.2), including the C8 split and F := G := R_f − 1 | Yes, before S5 | B-20, V2-D10 |
| FD-13 | V2-D1/D3/D4 defaults (§9.4) | As listed | S5 gate |
| FD-14 | Committee rule: bounded, weight-only, or replacement successor | Bounded, floor W/128, cap 64; **needs explicit owner approval** | B-22, V2-D5 |
| FD-15 | Contracts: permanent conflict halt; replacement-successor and succession paths | Raise with the contracts spec owner | B-9 |
| FD-16 | Signer participation incentive | Owner ruling before S5; no default amount | V2-D4 |

---

## 16. Governing text to reconcile (finality items)

Each needs a reviewed commit first; architecture §16 item numbers in
brackets.

| Text | Needed |
|---|---|
| `CLAUDE.md:87-88` (rule 3); `CLAUDE.md:12-13` [2] | One override: H1 with its escape, H2, the fresh lineage, B-22, B-37 and the bridge V2 regime |
| `doc/design/b3-cross-chain-finality-v1.md:3-11`, `:172-178` [9] | V2 beside a frozen V1; H1 ends the one-stake-universe identity and handover-gated production; bounded committee; row 175's re-bootstrap as §6; journals per lineage; B-23; B-37 |
| `src/wallet/wallet.h:1039-1067` (derived and imported keys) [10] | B-37 signer convention (§6.4) |
| `doc/design/b3-master-handoff.md:106`; `doc/design/b3-architecture-contract.md:609` [5] | 3-of-4 kept; it installs a recent fresh-lineage epoch, not Set_0 |
| Proposals to amend: `b3-flowmesh-v2-stage0.md:43`, `:69`, `:709`, `:711` [7]; `v2-stage0-r1-consensus.md:370-376`, `:404-480` (incl. `:407`), `:466-467` [8] | §9.2 |

---

## 17. Open items and owner reads

- **O2** lineage state, Set_3 size, weights (for the W/128 floor), online and
  unstaked members; **O8** block spacing (architecture §19).
- **B-37** (§6.4): the binding kind, ever-bound index, binding windows,
  carry-over, sit-out cost and the double-binding rule.
- **FD-15** (contracts spec §3.4, §14): whether a conflict halt should stop
  the verifier for good, and a replacement-successor or succession path.
- **Owner rulings:** B-11's alternative (FD-11), B-22's explicit approval (FD-14),
  FD-7, FD-12, FD-16; perhaps a pilot reserve ceiling given §10.1 (B-14 has
  none).
- Unmeasured [H]: PBFT round time, verifier V2 gas, accumulator cost,
  escape fork choice.
- Social: Set_3 cooperation for H1; committee liveness.

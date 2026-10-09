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

---

## 18. Implementation notes (S3 fresh-lineage hardening)

This section records what the S3 fresh-lineage hardening slice built on
`feat/flowmeshV2-dev-claude` on top of `965e23c`. It is regtest-only and
implements §6.4, §6.5 and architecture B-37. It is not an owner ruling and
changes no text above. §18.10 lists, as *pending owner review*, every
choice that the sections above leave open and every departure from a
sibling document (solve-and-report). References name files and symbols,
not lines.

### 18.1 Opt-in knob; what stays unchanged

- **`-b3freshkeyheight=<R_key>`** (`src/chainparams.cpp`,
  `src/chainparamsbase.cpp`, `src/kernel/chainparams.cpp`) sets
  `Consensus::FreshLineageParams::key_height`, and
  `Params::FreshKeyRulesActive(h)` holds from R_key. The knob requires
  `-b3freshlineageheight`, and through it `-b3modernregtest -b3flowmeshtest`.
  Every other chain refuses it as "regtest-only". R_key must satisfy
  1 ≤ R_key ≤ R_f − 2 and lie above the legacy final height, so the genesis
  window [R_key, R_f − 1] holds at least two blocks.
- **Unset, the default:** the staging lineage of `965e23c` is unchanged.
  Its committees carry the V1 FINALITY_KEY keys and the fresh key index is
  never stepped. Mainnet and testnet set no `fresh_lineage`, so
  `FreshKeyRulesActive` is false at every height there. On a chain without
  R_key, a policy-version-2 cell is still an "inactive metadata policy". A
  type-12 record is now refused as `mpa-inactive-type` (before:
  `mpa-unknown-type`), so both stay invalid.
- V1 bindings, the V1 signer, the V1 journals and the producer snapshots are
  untouched. `MatchFinalityKeyPairs` skips policy-version-2 cells, and the
  V1 binding index never reads the fresh one.

### 18.2 Byte layouts

| Object | Layout |
|---|---|
| Binding cell | Metadata cell, policy type 7 (FINALITY_KEY), **policy version 2**, zero value. Commitment = validator_key (x-only, 32). Params (56) = bls_pubkey 48 ‖ target_epoch u64 BE. V1 keeps policy version 1 |
| Evidence | **MPA type 12** FRESH_FINALITY_KEY_EVIDENCE, version 1, exactly 248 bytes: validator_key 32 ‖ bls_pubkey 48 ‖ target_epoch u64 BE ‖ bip340_sig 64 ‖ pop 96 (the identity-bound proof of possession below). Verify cost 700, the same as type 5 (`src/modern/payload_cost.h`). Type 12 is an MPA record only (never a standalone creation action); its number is frozen and append-only (`src/modern/creation_action.h`) |
| Bind digest | TaggedHash(`B3/FINALITY/FRESHBIND/V1`) over chain_domain 32 ‖ validator_key 32 ‖ bls_pubkey 48 ‖ target_epoch u64 BE, BIP340-signed by validator_key. V1's tag is `B3/FINALITY/BIND/V1`, so a signature of one kind never authorizes the other. The digest binds no lineage id (unknown before R_f), and there is no sequence and no revocation |
| PoP digest | TaggedHash(`B3/FINALITY/FRESHPOP/V1`) over the same four fields, signed by the **fresh BLS key itself** with the signature suite (`bls::SIG_DST`): `pop` = BLS signature over this digest. The message contains the key, so it proves possession of the secret as the IETF PoP does (rogue-key safety for the committee aggregates; no vote digest uses this tag). Unlike the IETF PoP (sign(sk, pk) under `POP_DST`, which V1's type-5 evidence carries) it names the validator, the chain and the epoch, so no copy serves another claim (§18.3) |
| Key store | `<datadir>/<net>/finality_signer/finality_fresh_keys_<chain_domain hex>_<validator hex>.dat`, integers LE: magic `B3FKEYS1` 8 ‖ version u8 = 1 ‖ chain_domain 32 ‖ validator 32 ‖ count u32 (≤ 4,096) ‖ count × {target_epoch u64 ‖ state u8 (1 HELD, 2 ERASED) ‖ bls_pubkey 48 ‖ secret 32 (big-endian scalar, zero when ERASED) ‖ bind_txid 32 (null until recorded)} ‖ SHA256 of all preceding bytes 32. Header 77 bytes, record 121 |
| `finsigv2` | Exactly 148 bytes, the `FreshLineageSig` serialization: lineage_id 32 ‖ epoch u64 LE ‖ height u64 LE ‖ member index u32 LE ‖ BLS signature 96 over digest V2 |

`MatchFreshFinalityKeyPairs` (`src/modern/finality_fresh_key.h`) pairs
cells and records one-to-one by (validator_key, bls_pubkey, target_epoch).
It fails closed with one of `fresh-key-cell-params`,
`fresh-key-duplicate-cell`, `fresh-key-duplicate-evidence`,
`fresh-key-orphan-evidence`, `fresh-key-cell-without-evidence` or
`fresh-key-evidence-malformed`. `CheckFreshKeyTransition` then checks, in
order: the fields agree, the key is nonzero, the BIP340 signature verifies,
the key decodes canonically, and the identity-bound PoP verifies.

### 18.3 Consensus checks structure; reuse is inert; the mempool refuses

- **Consensus** (`src/validation.cpp`, gated). ContextualCheckBlock,
  ConnectBlock and mempool PreChecks run the matcher
  (`bad-fresh-finality-key-binding`). With R_key configured, ConnectBlock
  also runs `VerifyFreshKeyBindingsInBlock`
  (`bad-fresh-finality-key-evidence`), and the mempool runs
  `CheckFreshKeyTransition` (`fresh-key-<check>`, TX_CONSENSUS). No other
  property of a binding invalidates a block.
- **Ever-bound index** (`FreshKeyBindingIndex`,
  `src/node/finality_fresh_key_index.*`). `FinalityTracker` keeps it in key
  mode only. It is stepped and rebuilt together with the tracker's stake and
  V1 binding trackers from the first modern block, and every fresh pair is
  re-verified. It keeps no undo trail: a disconnect marks the tracker dirty
  and the next sync rebuilds all three from H+1. Every nonzero key claimed
  under either kind is recorded once, and **the first claim wins**, in block
  and transaction order. Inside one transaction, V1 pairs come first (in V1
  matcher order) and fresh pairs follow in matcher key order, which is not
  output order. A V1 key that its validator later moved off or revoked stays
  bound.
- **Inert reuse.** A fresh claim of an already bound key is recorded
  nowhere and seats nobody. So neither a V1 key nor an earlier fresh key
  ever enters a fresh committee. A V1 claim of a fresh-bound key follows the
  unchanged V1 rules.
- **No front-running (Codex review, blocking).** The first draft carried
  the bare IETF PoP, a signature over the 48 key bytes only. Once an honest
  binding was in the mempool, anyone could copy its key and PoP into a
  fresh binding under any BIP340 key, or into a V1 FINALITY_KEY binding,
  get it mined first, and burn the key (first claim wins): the member sat
  out, without an alert. Front-running every genesis binding would have
  kept the lineage from bootstrapping; front-running later ones would have
  forced carry-over forever, or concentrated committee weight. The fix is
  the identity-bound PoP (§18.2): the key signs (chain, validator, key,
  epoch). A fresh copy under another identity, epoch or chain fails
  `fresh-key-bad-pop` (consensus); a V1 copy fails V1's `bad-pop`, because
  no fresh binding publishes the bare IETF PoP that V1 demands. So only a
  holder of the key's secret can claim a fresh key, under either kind.
  Tests: `pop_digest_layout_and_separation`,
  `front_running_copies_of_a_pending_binding_are_refused` and the chain
  copies (fresh and V1) in `mempool_and_block_wiring`
  (`finality_fresh_key_tests`). A key claimed first elsewhere anyway (a
  leaked secret) is alerted and replaceable (§18.6, §18.7).
- **Mempool policy** (TX_NOT_STANDARD, key mode only,
  `CheckFreshKeyBindingPolicy`). It refuses with `fresh-key-ever-bound` (the
  key was bound before under either kind, or the transaction claims it
  twice), `fresh-key-binding-outside-window`,
  `fresh-key-lineage-unavailable` or `fresh-key-state-unavailable`. It does
  not consult bindings still waiting in the mempool; with identity-bound
  proofs a pending binding can only compete with another claim by the
  key's own holder.
- **Why inert rather than invalid.** The only context a binding's validity
  depends on is the activation height R_key. No in-block conflict can arise,
  so block assembly (`src/node/miner.cpp`) is unchanged. Type-11 certificates are
  judged against the committees, and every node derives those committees
  deterministically from the same blocks.

### 18.4 Binding windows

A block at height n counts a binding for exactly one target epoch
(`FreshBindingWindowAt`). The committee rule reads the same windows
(`FreshSeatWindow`).

| Target | Counted if included in | Fixes |
|---|---|---|
| 0 (genesis) | [R_key, R_f − 1] | Set′_0 and Set′_1 (§6.4 rule (3)) |
| 1 | Never | Nothing: Set′_1 shares the genesis binding |
| t ≥ 2 | [start_(t−2), start_(t−1) − 1], i.e. during fresh epoch t − 2 | Set′_t, at the rotation into t − 1 (rule (2) with k + 1 = t) |

Below R_key the window is `not-active`. From R_f, it is
`lineage-unavailable` unless the lineage is bootstrapped and unbroken. A
binding included outside its window is not counted, yet its key is still
burned (ever bound).

### 18.5 Committee(Snapshot) filter, void and carry-over

`BuildFreshCommittee` and `FinalityTracker::FreshCommitteeAt` implement
this. `ProjectFreshLineage` applies them only when the tracker passes
`committee_at`, which it does in key mode.

- Snapshot(h) membership is V1's, unchanged: positive whole stake weight and
  a non-revoked V1 FINALITY_KEY binding. A member sits **on its fresh key,
  with its stake weight**, only if exactly one counted binding names the
  committee's epoch inside the window. With no such binding it sits out;
  with two or more, its seat is **void** for that epoch (`FreshSeatFor`).
  The committee is built over a temporary binding index that holds only
  the seated fresh keys, so `ValidatorSetSnapshot::Build` and
  `src/node/validator_set.*` are unchanged.
- Genesis uses the request {snapshot R_f − 1, target 0, window from R_key}.
  Set′_1 is Set′_0's members stamped 1. Below `min_finality_set` the lineage
  stays unbootstrapped, with no fallback to V1 keys.
- Rotation into epoch e at height h uses the request {h − 1, e + 1, window
  from start_(e−1)}. Below the floor the existing carry-over applies with
  Set′_e's members **and keys** (rule (4)), and bindings for the skipped
  epoch go unused.
- B3 still does not enforce the bridge's qualification of 4-64 members and
  weight ≥ 900 (B-22 is out of scope, §18.11).

### 18.6 Signer, store and erasure

- **Keys.** Each key comes from 32 bytes of `GetStrongRandBytes` through the
  IETF KeyGen (`bls::SecretKey::FromIKM`). Nothing reads the wallet
  identity secret, so a wallet backup cannot regenerate a fresh key, unlike
  `DeriveFinalityBlsKey`. The store (`FreshKeyStore`,
  `src/node/finality_fresh_key_store.*`) refuses to generate a second HELD
  key for a target epoch, because a second binding would void the seat.
  - It fails closed on any byte it cannot verify: magic, version, checksum,
    length, chain domain, validator, state, a non-canonical or duplicate
    key, a HELD secret that does not match its key, or an ERASED record with
    a nonzero secret byte.
  - Each write runs under a process-wide mutex and first re-reads the file
    it replaces. It then writes a temporary file (mode 0600) and runs
    FileCommit, RenameOver and DirectoryCommit.
  - Secrets live in locked memory and are cleansed when freed. Records are
    never removed.
- **Signing.** In key mode `FinalitySigner::SignFresh` takes a seat's
  secret only from fresh keys, never from a V1 key. A persistent signer
  re-opens its store on every pass, so keys the wallet bound meanwhile
  appear. An offline or test signer uses `SetFreshKeys`, which takes at most
  16 keys. Without key mode, signing uses the V1 keys as before.
- **Erasure condition** (`FreshKeyRetiredSince`, `FreshKeyErasable`). Take
  a tip in fresh epoch e of a bootstrapped lineage and a HELD key for target
  t. The key is erased when all three hold: t ≤ e + 1; neither Set′_e nor
  Set′_(e+1) seats it; and tip − since ≥ horizon. Here since =
  start_(e−1) when e ≥ 1, t ≤ e and Set′_(e−1) does not seat the key
  either; otherwise since = start_e.
  - Why this matches §6.4: each epoch start lies above the inclusion of the
    certificate that handed over out of the previous epoch. So the rule is
    §6.4's "certified on B3 and deep", with the depth counted from the start
    of the epoch whose entry retired the key.
  - horizon is the modern reorg horizon (`FreshKeyErasureHorizon`: 1,440 on
    mainnet, `-b3reorghorizon` on regtest), else E.
  - Consequences: the genesis key goes at start_2 + horizon. A key that was
    never seated (no counted binding, a void seat, or a skipped carry-over
    epoch) goes once its committee is fixed and deep. A carried-over key
    stays while a committee seats it, and a key for a later epoch stays.
  - Erasure zeroes the secret in the rewritten file. Filesystem remnants and
    snapshots are outside the node's control.
- **Status and alerts.** Every pass computes them before the journal safety
  check: the signer's keys with their seated epochs, and the seats in
  Set′_(e−1), Set′_e or Set′_(e+1) without a held secret (a Set′_(e−1) key
  that the policy erased does not count as missing). It raises an alert
  for each counted chain binding of this validator whose key the store
  neither holds nor erased (an old backup or a leaked identity key), and for
  each void seat. Each alert is logged once while it persists. A current
  seat without a held secret, or a store that cannot be read, is reported
  as a recoverable `last_error`. A held key that the chain recorded under
  another first claim (`FreshKeyBindingIndex::EverBound` without
  `OwnFreshEpoch`: another validator's binding of either kind) raises its
  own alert: this validator's binding of it is inert, and only a holder of
  the secret could have made the claim.
- **Unavailable committees.** `FinalityTracker::FreshCommitteeAt` reports
  a request whose inputs are not at the snapshot height (a tracker bug) as
  UNAVAILABLE, never as "nobody seated". `ProjectFreshLineage` then resets
  the fresh state and returns false, `Projected()` sets
  `fresh_unavailable`, `ApplyModern` fails (the tracker stays dirty and
  rebuilds), and `CheckBlockCertificate` / `JudgeCandidateCertificateV2`
  answer `finality-state-unavailable`, so no certificate is ever judged
  against a guessed (carry-over or unbootstrapped) committee.
- **Journal catch-up** (`FinalitySignerStore::CommitLineageCatchUp`).
  It applies to FRESH_LINEAGE_V2 journals only; V1 journals refuse it. The
  problem it solves: a member that sat out an epoch signed nothing in it, so
  its ancestry lock still names its last seated epoch, and its next seat
  would not follow that lock's successor set. The fix: when the lock's block
  is on the active chain and an included fresh certificate of a later epoch
  lies above it, the lock moves to that certificate. The certificate is
  rebuilt and matched against its certificate id first, and the last actual
  vote is kept.

### 18.7 Wallet and RPC

- **`bindfreshfinalitykey`**, called as
  `bindfreshfinalitykey ( epoch {"broadcast":bool,"reuse_key":"v1"|"epoch:<n>"} )`
  (`src/wallet/rpc/staking.cpp`).
  - It refuses before touching the wallet: without key mode (every shipped
    network) nothing is created or written, and a wallet without a
    validator identity is refused rather than given one. It then needs an
    unlocked wallet and the epoch that the next block's window counts; any
    other epoch is refused.
  - It is refused while this node's store holds a HELD key for that epoch,
    unless that key can never seat this validator: no binding transaction
    was ever recorded for it (the RPC records the txid before broadcasting
    or returning hex, and publishes nothing if it cannot, so such a key
    was never published), or the chain recorded another first claim of its
    key (the chain interface probes the store's keys). Such a key is
    erased first and listed under `replaced`.
  - It generates the key durably in the node's store, which it finds in
    the signer directory through `interfaces::Chain::freshFinalityKeyStatus`.
  - It signs the bind digest with the wallet's validator secret, adds the
    PoP, builds the cell plus the type-12 record, and records the txid in
    the store.
  - It warns when the validator has no active V1 binding. It erases a
    generated key whose transaction cannot be built or recorded (reopening
    the store once if a signer pass rewrote it).
  - `broadcast=false` returns the hex. `reuse_key` serves regtest drills
    only and needs `broadcast=false`: it rebinds the wallet's V1 key or a
    stored fresh key to show `fresh-key-ever-bound`.
- **`getfreshfinalitykeys`** joins the store's public records with the chain
  bindings and seats, and returns `foreign_bindings`, `missing_seats` and
  `alerts`; a store key claimed first elsewhere carries
  `claimed_elsewhere_height` and an alert. Without the knob it returns only
  `configured: false`.
- **`getfinalitylineage`** gains a `fresh_keys` object in key mode only. It
  holds R_key, `next_binding_window`, `next_binding_epoch`, `window_start`,
  the bindings with `counted` and `void`, and Set′_(e−1), Set′_e and
  Set′_(e+1) on their fresh keys. `local_signer` gains `fresh_keys`,
  `missing_seats` and `alerts`.
- The V1 RPCs `bindfinalitykey`, `importfinalitykey` and `startstaking` are
  unchanged. The wallet file holds no fresh secret: a restored backup can
  still bind a key (§6.4's limit), but it cannot sign for any epoch.

### 18.8 P2P transport (`finsigv2`)

- **Gate.** A node handles and sends `finsigv2` only where its chain
  configures a fresh lineage (regtest `-b3freshlineageheight`, with or
  without R_key). Every other node ignores the message before deserializing
  it, applies no penalty and sends nothing. The type is appended to
  `ALL_NET_MESSAGE_TYPES`, so `getpeerinfo` counts it by name. There is no
  protocol-version gate.
- **Receive path,** in order:
  1. A size other than 148 bytes is ignored.
  2. The vote takes a token from both the peer and the global wire bucket,
     or from neither.
  3. The vote is deserialized.
  4. Under cs_main the bridge and finality trackers sync, as for `finsig`.
  5. `pool.Fresh().Submit` charges the peer and global verify buckets once,
     immediately before BLS decoding and verification. It never charges a
     cheap reject or a known duplicate. A refused charge returns
     `verification-deferred`.
  6. On ACCEPTED, the vote floods once to every other connected, unpaused
     peer.

  There is no holding area. A vote that is too shallow, not on this tip's
  grid or over budget is dropped, and the sender's replay retries it.
- **Buckets** (`src/node/finality_fresh_transport.h`). They are separate
  from V1 `finsig`'s buckets but repeat V1's values, because committees are
  still full snapshots (MAX_FINALITY_SET = 8,192). A refusal costs nothing
  and never disconnects, and a refill never moves the clock backwards.

| Bucket | Rate per second | Capacity |
|---|---|---|
| Per-peer wire | 128 | 2 × MAX_FINALITY_SET |
| Global wire | 512 | 4 × MAX_FINALITY_SET |
| Per-peer verify | 32 | MAX_FINALITY_SET |
| Global verify | 128 | 2 × MAX_FINALITY_SET |

- **Replay** (`FreshVoteReplay`, one per connection).
  - Each connection gets at most one batch per TICK = 1 s, of at most 64
    votes, newest checkpoint first.
  - Batches come from one shared export of the pool's verified votes
    (`FreshVoteExport`, which hashes each vote id once per refresh, not per
    peer per tick). The export is refreshed on every tip-hash change,
    otherwise at most once a second.
  - A vote goes out unless it was sent on this connection less than
    RESEND = 15 s ago.
  - The source (after ACCEPTED or DUPLICATE) and every flood recipient are
    marked as sent, so no vote echoes back before RESEND.
  - A remembered id is pruned only once its vote is out of the export AND
    it was sent or marked at least RESEND ago. So a vote a peer relayed to
    us is not echoed back even while the lagging export does not contain it
    yet (the first draft dropped the mark at the next stale export). Memory
    stays near two full pools; `MarkSent` stops at 8 checkpoints ×
    MAX_FINALITY_SET.
  - Votes from the local signer and from the hidden RPC go out within one
    TICK. `src/node/staking.cpp` is unchanged.
- **Hidden RPCs.** `getfinalitylineagevotes` and `submitfinalitylineagevote`
  stay hidden, for drills. The functional harness (`FreshVoteShuttle` in
  `test/functional/test_framework/b3bridge.py`) uses gossip by default and
  keeps an `rpc` transport for drills. RT4, RT4a, RT4c, RT4d and
  `feature_b3_fresh_lineage.py` move votes by gossip and assert `finsigv2`
  traffic; `feature_b3_fresh_lineage.py` keeps one hidden-RPC drill.

### 18.9 Tests

- **Unit suites:**
  - `finality_fresh_params_tests`: the knob.
  - `finality_fresh_key_tests`: codec, matcher, checks, registry, ever-bound
    index, windows, mempool policy, seat rules, committees, store, erasure
    truth table, journal catch-up, signers and RPC status on chain fixtures.
  - `finality_fresh_transport_tests`: message layout, buckets, replay
    (including a stale export), and the `finsigv2` path through a real
    PeerManager, with and without the lineage; on a frozen clock the gate
    charges no budget, the per-peer wire bucket drops a valid vote after
    its capacity of cheap ones, and the per-peer verification bucket defers
    one after its capacity of BLS failures until a refill.
  - The wallet suite `finality_fresh_key_wallet_tests`: keys are independent
    of the wallet and its backups.
  - `finality_v2_tests`: updated for type 12.
- **Functional:** the new `feature_b3_fresh_lineage_keys.py` (extended
  list). It runs five validators staking 40/30/20/10/10 with
  `-b3minfinalityset=4`, R_f = 262, R_key = 241 and horizon 40, plus a sixth
  node holding a restored backup of n0's wallet. It covers rotation to new
  keys, an old backup that cannot sign, a void seat, erasure, restart and
  `-reindex`, with no vote moved by RPC. `feature_b3_bridge_rt4a_round_trip.py
  --freshkeys` (its own test_runner entry) runs the bridge V2 round trip on
  genesis fresh keys bound between R_key = 241 and R_f − 1, so Verifier V2
  is initialized with, and follows the handover to, a fresh-key committee.

### 18.10 Implementation choices pending owner review

1. **The knob is opt-in.** The regtest plan's startup line and knob row give
   RT4 `-b3freshkeyheight=241`, but RT4, RT4a, RT4c, RT4d and
   `feature_b3_fresh_lineage.py` run without it. The reason is RT4c: its
   validators bind the certify tool's public test keys as V1 keys, so that
   the tool can sign as B3's committees. Per-epoch keys run in
   `feature_b3_fresh_lineage_keys.py` and in the extra RT4a entry
   `--freshkeys`, which takes Verifier V2 through a fresh-key genesis and
   handover (§18.9); no RT4 run yet relays a rotation to newly bound keys or
   a void-seat member-count change to the verifier.
2. **Never-bound-twice is not a validity rule.** §6.4 rule (1) ("refuse any
   key ever bound") is implemented as mempool policy plus consensus
   inertness, not as block invalidity (§18.3).
3. **Claim order inside one transaction:** V1 pairs first, then fresh pairs
   in matcher key order.
4. **A fresh binding supplies only the key.** The seat still requires
   Snapshot(h) membership, through a live V1 binding and active stake.
5. **No V1-key fallback at genesis.** A genesis committee below the floor
   leaves the lineage unbootstrapped.
6. **Erasure depth.** It counts from the start of the epoch that retired the
   key, not from the current epoch's start. Counting from the current start
   would never reach the depth on a timely chain whose horizon is not below
   E (on mainnet, E = horizon = 1,440).
7. **The store's keying.** It is keyed by (chain domain, validator), not by
   lineage id, because genesis keys exist before the lineage id does. It
   does not store the proof of possession that node changes §7.7 lists:
   the PoP is on chain in the type-12 record and can be recomputed.
8. **The journal catch-up** across epochs a member sat out (§18.6).
9. **`finsigv2` design.** Its budgets repeat V1's, and it floods once plus
   replays, with no holding area.
10. **Identity-bound proof of possession** (§18.2, §18.3). The type-12
    `pop` is the fresh key's BLS signature over TaggedHash(
    `B3/FINALITY/FRESHPOP/V1`, chain_domain ‖ validator_key ‖ bls_pubkey ‖
    target_epoch) with the signature suite, not a dedicated DST: the frozen
    BLS wrapper (`src/crypto/bls.h`) exposes no DST parameter and is
    outside this slice, and the unique tag separates it from every vote
    digest. The layout stays 248 bytes. A V1 claim of a fresh key is not
    tracked per validator: it is impossible without the key's secret, since
    no fresh binding publishes the bare IETF PoP.
11. **Journal keying.** §6.4 "Journals" asks for a store keyed by (lineage
    id, key). In key mode the fresh journal stays keyed by (lineage id,
    validator) across all of a validator's per-epoch keys, with the
    cross-epoch catch-up (`CommitLineageCatchUp`, §18.6). One watermark per
    validator is stricter than one per key (it also orders votes across
    that validator's keys), so it is safe.
12. **Replacing a held key** (§18.7). `bindfreshfinalitykey` replaces a
    held key for the window's epoch only when it was never published or its
    key was claimed first elsewhere. A deep reorg that removed that other
    claim after the replacement could make both of the validator's
    bindings count and void its seat; that needs the key's secret to have
    leaked first, which already calls for a new identity.

### 18.11 What stays open

- **B-22**, the bounded committee and forwarders, is out of scope. Fresh
  committees are full snapshots, and the buckets are sized for 8,192
  members.
- **B-37 and B-22 together.** Under B-37 who is seated depends on every
  member binding again each epoch by hand (no automatic rebinding), so
  seated committees shrink and vary more than V1 snapshots. Carry-over
  applies only below `min_finality_set`, not below the bridge's
  qualification (4-64 members, weight ≥ 900). A seated committee that B3
  accepts but Verifier V2 does not qualify is therefore more likely: it
  stalls withdrawals, and strands them if the verifier's set expires.
  Proposed before S5 (not implemented, it changes the committee rule):
  on chains with bridge V2, carry over also when the seated set would not
  be bridge-qualified (regtest-gated first), or land automatic rebinding
  together with B-22.
- **Automatic rebinding.** An operator calls `bindfreshfinalitykey` once
  per window; the staking loop never binds.
  - A member that misses a window sits out that epoch, unless a carry-over
    keeps its seat.
  - A binding still in the mempool when its window closes is not evicted.
    If it is mined later, it is inert and burns its key.
- **Encryption at rest.** The store holds raw secrets in a 0600 file,
  outside wallet encryption and wallet backups. Moving a validator to
  another datadir moves no fresh key: the operator either copies the store,
  or binds anew and sits out meanwhile.
- **OI-7 for P3-R1.** The windows and the erasure condition use V1's
  handover-gated staging epochs and Snapshot(start − 1), whereas P3-R1
  snapshots at the terminal checkpoint
  (`doc/design/v2-stage0-r1-consensus.md:336-347`). Rules (2)-(4) and the
  erasure condition need restating for P3-R1 (§9.2) before S5.
- **B-37's limits, unchanged.**
  - The identity key in every backup can bind a key for a coming epoch. If
    the member also binds, the seat is void; if not, the backup holder takes
    the seat.
  - The alerts detect this but do not prevent it; a leak calls for restaking
    under a new identity.
  - Sit-outs cost committee weight, which the S5 liveness rehearsal must
    measure.
  - Erasure protects past epochs only, and only where the signer runs.
- **Mainnet.** No shipped network sets R_key. Mainnet gets per-epoch keys
  only with P3 (B-7), after OI-7, a reviewed hard fork and an owner ruling.

---

## 19. Implementation notes (S1 H1/H2, regtest)

This section records what the H1/H2 chain-health slice built on
`feat/flowmeshV2-dev-claude` on top of `8e3081b`, under owner ruling R8
(`doc/design/b3-bridge-v2-owner-rulings-2026-10-03.md`). It is regtest-only
and implements §5.1-§5.3 and FD-1 to FD-4 as far as regtest can. It is
**not an owner ruling** and changes no text above. §19.8 lists, as *pending
owner review*, every choice §5 leaves open and every departure from the
regtest plan (solve-and-report). References name files and symbols, not
lines.

### 19.1 Knobs, guards and what stays unchanged

| Knob | Sets | Guards |
|---|---|---|
| `-b3h1height=<R_h>` | `Consensus::Params::h1` = `H1Params{R_h, R_escape}`; `H1RulesActive(h)` holds from R_h | Integer ≥ 2; R_h > M ("must be above the first modern-PoS height M=…") |
| `-b3h1escaperounds=<n>` | R_escape; default `H1Params::DEFAULT_ESCAPE_ROUNDS` = 2,880 (24 h at 30 s rounds); `H1EscapeRound(h, round)` | 1 ≤ n ≤ 99,999; needs `-b3h1height` |
| `-b3h2checkpoint=<height>:<hash>` | Repeatable; each pin joins `Consensus::Params::modern_checkpoints` | Height in the MODERN era (above the legacy final height); 64 hex digits in display order, as `getblockhash` prints; a duplicate height is refused |

- **Common guards.** All three knobs need `-b3modernregtest -b3flowmeshtest`
  (`ReadRegTestArgs`, and again in the kernel's `RegTest()`), as regtest
  plan §3.3 requires of its startup knobs. Every other chain refuses each
  one as "regtest-only" (`CreateChainParams`). Negation is refused, and the
  knobs are DEBUG_ONLY (`src/chainparamsbase.cpp`). Files:
  `src/consensus/params.h`, `src/kernel/chainparams.{h,cpp}`,
  `src/chainparams.cpp`, `src/chainparamsbase.cpp`.
- **Not provided.** The regtest plan's `-b3regtestwithholdcertificates`
  is not in this build; the argument parser refuses it as an unknown
  parameter (§19.8 item 1).
- **E** is not a new parameter. It is `ModernPosParams::finality_epoch_blocks`
  (1,440 on mainnet, `-b3epochlength` on regtest).
- **Unchanged.** Mainnet, testnet, testnet4 and signet set no `h1`, so
  `H1RulesActive` is false at every height. Mainnet's `modern_checkpoints`
  is exactly {810,001, 811,641} with their hashes, and the other networks'
  tables stay empty. A regtest node without `-b3h1height` has no `h1`
  either: `State::producers` is never set, and every producer path is V1's
  byte for byte (H1-R1).
- **Nothing new on disk or on the wire.** There is no new byte layout, P2P
  message, persisted file or index. The H1 state lives in memory and is
  rebuilt from blocks, like the V1 tracker. BLS, digests, journals,
  carriers, certificates, the fresh lineage, the bridge and the Ethereum
  code are untouched (H1-R6). FlowMesh reads no producer set and is
  unchanged.

### 19.2 The rule as implemented

`node::FinalityTracker` (`src/node/finality_tracker.{h,cpp}`) keeps a
`ProducerSchedule` {epoch, epoch_start, current, next, last_escape_height,
escapes} in `State::producers`. It is set only from R_h and is never
written into, or read from, a V1 field (H1-R3). `ProjectProducers(h, round)`
derives it for the block at h = synced height + 1:

```text
ProjectProducers(h, round):            # H1 active at h
  require ProducerInputsReady(h)                    # else nullopt (fail closed):
                                                    # R_h > M, synced_height == h - 1,
                                                    # a stored schedule above R_h
  if h == R_h:                                      # bootstrap
    p = {epoch 0, start R_h, last_escape -1, escapes 0}
    p.current = SetOrCarry(0, V1 State::current after block R_h - 1, min_finality_set)
    p.next    = p.current re-stamped 1
  else:
    p = stored schedule
    if h >= p.start + E:                            # rotation, no certificate
      p.epoch += 1; p.start = h
      p.current = p.next
      p.next    = SetOrCarry(p.epoch + 1, p.current, min_finality_set)
  if round >= R_escape:                             # escape (FD-2), applied last
    s = SetOrCarry(p.epoch + 1, p.current, 1)       # no floor: Snapshot(h - 1) if non-empty
    p.epoch += 1; p.start = h
    p.current = s; p.next = s re-stamped p.epoch + 1
    p.last_escape = h; p.escapes += 1
  return p

SetOrCarry(epoch, fallback, floor):
  Snapshot(h - 1) stamped epoch,  if it has >= max(floor, 1) members
  else fallback re-stamped epoch  (carry-over; V1's with floor = min_finality_set);
  null if no fallback
```

- **Snapshot.** This is V1's rule, unchanged: ACTIVE stake joined with a
  non-revoked FINALITY_KEY binding. `src/node/validator_set.*` is not
  touched. `SyncedTipSnapshot` memoizes Snapshot(h − 1) per synced tip, so
  validation, the assembler, the staking loop and the RPC build and
  BLS-decode it once per parent.
- **`Projected(h, params, round)`** runs V1's projection unchanged, then
  sets `producers` from R_h, then runs the fresh lineage's projection
  unchanged.
- **`SetInForceAt(h, params, round)`.** Below R_h, or without `h1`, it runs
  the V1 body byte for byte and ignores the round. From R_h it returns
  `ProjectProducers(h, round)->current`, or null.
- **The escape has no floor.** §5.1 gives the escape Snapshot(parent);
  the implementation takes it whatever its size and carries the set
  otherwise in force over only when the snapshot is empty. Bootstrap and
  rotation keep V1's `min_finality_set` carry-over (§19.8 item 2).
- **`ProducerScheduleAvailable(h)`** is `ProducerInputsReady(h)`, the same
  private predicate `ProjectProducers` checks, so the two cannot drift. It
  returns false only when H1 is active at h and one of these holds: the
  tracker is not at h − 1, a height above R_h has no stored schedule, or
  R_h ≤ M. In that case
  `Chainstate::ModernEligibilityWeights` returns nullopt, and validation
  reports `state.Error("modern-PoS validator set is unavailable")`, never
  `bad-pos-ineligible`.
- **Escapes come from block data only (H1-R4).** The round is
  `modern::DecodeModernPosRound(parent time, block time)`.
  - `ConnectBlock` passes it to `ModernEligibilityWeights`. A block time
    that is not exact for a round gives no round: the normal set is read,
    and `CheckModernPosBlock` refuses the block with `bad-pos-time`, as
    before.
  - `ApplyModern` decodes the same round. It fails, leaving the tracker
    dirty for `Sync` to rebuild, when a connected block has no exact round
    or the schedule is unavailable. It never stores a guessed schedule.
  - So restart, `-reindex-chainstate` and `-reindex` reproduce every
    rotation and escape.
- **Order within one height.** The bootstrap or rotation applies first,
  then the escape. An escape at a rotation height therefore advances the
  epoch twice, and the escape set replaces the rotation's next set. Both
  come from the same snapshot.
- **V1 from R_h.** The V1 tracker keeps computing epochs, handovers,
  certificates, the break, the finalized checkpoint and reorg pins exactly
  as before.
  - Under an intact lineage, V1 certificates stay the checkpoint authority
    while producers follow the schedule.
  - On a broken lineage, V1 stays frozen.
    `BridgeWithdrawalValidatorSetsReady` reads only V1 fields, so burns
    stay closed.

### 19.3 Call sites (H1-R2)

Every path that decides who may produce reads
`Chainstate::ModernEligibilityWeights` or `FinalityTracker::SetInForceAt`.
Both are H1-aware, so these paths change together.

| Caller | What it decides | From R_h |
|---|---|---|
| `Chainstate::ModernEligibilityWeights` (`src/validation.{h,cpp}`) | (w, W) for a key at parent + 1 | Checks `ProducerScheduleAvailable`, then calls `SetInForceAt(parent + 1, round)`. `round` is a new defaulted parameter |
| `Chainstate::ConnectBlock`, modern-PoS branch | Block validity | Passes the block's decoded round, so an escape block is judged against the escape set |
| `BlockAssembler::CreateNewBlock` (`src/node/miner.cpp`) | Template eligibility and round search | Reads the normal pair and the escape pair (round = R_escape), and uses the escape pair for rounds ≥ R_escape. Throws "not in the active validator set" only when both weights are 0. An explicit `modern_pos_round` needs w > 0 in that round |
| `StakingLoop::ThreadLoop` (`src/node/staking.cpp`) | Coordination and the preferred-proposer slot | Reads both weight pairs and both sets: `SetInForceAt(next)` and `SetInForceAt(next, R_escape)`. Ranks each round over the set that governs it. With no set for a round, it waits for the next round |
| `StakingLoop::FillChainFacts` | `getstakinginfo` `active_weight` and `total_active_weight`, and through them the Qt validator page | Normal-round weights. The escape weight is not reported (§19.8 item 9) |
| `getfinalitystatus.producers` (`src/rpc/blockchain.cpp`) | The H1-R5 report | Reads `Projected(tip + 1).producers`, `SetInForceAt(tip + 1)` and `SetInForceAt(tip + 1, R_escape)` |
| `getfinalitystatus.set0_preview`, `getfinalityset` and `ChainImpl::finalityStatus` (`src/node/interfaces.cpp`) | The V1 Set_0 hand-off through `SetInForceAt(M)` | Unchanged. R_h > M is enforced, so `SetInForceAt(M)` is always V1's |
| V1 readers of `Projected()` and `Current()`: the V1 signer, certificate judging, bridge burn readiness (`src/validation.cpp`, `src/node/miner.cpp`, `src/node/interfaces.cpp`, `src/node/bridge_state.cpp`), `getfinalityset`, `getfinalitystatus.validator_set` and the Qt finality-membership fields | V1 finality only, never production | Unchanged. They read V1 fields only, never `producers` |

The regtest production pause (`preserve_regtest_finality` in
`CreateNewBlock`) also changes: from R_h the V1 lineage never pauses local
production, because producers no longer depend on it (regtest plan §3.6).

- A LINEAGE_BROKEN plan becomes ALLOW.
- A REQUIRE_HANDOVER plan (an intact lineage's last carrier) becomes ALLOW
  too. The assembler still includes a pooled current-epoch certificate,
  as it does at every height, but with none it builds the carrier anyway
  and the V1 lineage breaks one height later, as it would on a network
  without this regtest-only policy. Below R_h both pauses are unchanged.
- Consequence: a regtest staker no longer waits at the carrier for a slow
  quorum after R_h. A fixture that needs an intact lineage after R_h must
  keep its signers live, which fixture I does (certificates arrive tens of
  blocks before the carrier).
- `src/node/regtest_finality_policy.h` is unchanged.

### 19.4 RPC (H1-R5)

`getfinalitystatus` gains a `producers` object. It is present only when
`h1` is configured, so never on a shipped network. Every field is
documented in its RPCResult.

| Field | Before R_h (`active` false) | From R_h |
|---|---|---|
| `activation_height`, `escape_rounds` | R_h, R_escape | Same |
| `active` | false | true (tip + 1 ≥ R_h) |
| `epoch`, `epoch_start` | Absent | The producer epoch of tip + 1 and its first height |
| `next_rotation` | R_h | epoch_start + E |
| `snapshot_height` | R_h − 1 | next_rotation − 1 |
| `stake_enters_at` | R_h | next_rotation + E. A stake that is ACTIVE and bound by `snapshot_height` produces from here, unless an escape comes first |
| `last_escape_height`, `escapes` | Absent | −1 and 0 until the first escape |
| `escape_time` | Absent | tip time + block interval + R_escape × round seconds: the exact time of an escape block on this tip |
| `set` {epoch, size, total_weight, set_hash} | The V1 set in force for tip + 1 | The producer set for tip + 1 in a normal round |
| `next_set`, `escape_set` | Absent | The next epoch's set, and the escape set on this tip |

- `validator_set` now reads "the V1 finality set in force (block
  producers: see producers when H1 is configured)".
- No RPC lists producer-set members: `getfinalityset` returns V1 sets only.
- Without `-b3h1height` the output is unchanged.

### 19.5 H2

- **No new mechanism.** An H2 pin is an entry in
  `Consensus::Params::modern_checkpoints`, with `Consensus::ModernCheckpointAllows`
  (`src/consensus/boundary.h`) as the only predicate. Three checks already
  enforce that table:
  1. Header admission (`ContextualCheckBlockHeader`) refuses a different
     block at a pinned height with `bad-modern-checkpoint`.
  2. `ConnectBlock` checks again, which also covers `-reindex-chainstate`.
  3. At startup, `src/node/chainstate.cpp` refuses an active chain that
     does not match: "does not match the hardened modern checkpoint at
     height …".
- **Effect.** Every branch that reaches a pinned height must contain the
  pinned block, so no branch that passes the pin can rewrite anything at or
  below it. It does not bound reorgs above the pin (§5.3).
- **Regtest.** `-b3h2checkpoint` adds pins at any modern height, including
  at or above R_h. RT1 pins 300 with R_h = 220; unit case (n) pins
  R_h + 1 from the knob's display-order hash.
- **Mainnet.** No entry was added. The §5.3 entry for the H1 release needs
  a recent block hash, observed independently and reviewed at release
  time, added beside 810,001 and 811,641 in `src/kernel/chainparams.cpp`. A
  follow-up release above R_h adds one past activation.
- 811,641 must stay in the table, because mainnet's finality signer
  recovery pin anchors there. Regtest configures no recovery pin, so an H2
  pin never anchors one.

### 19.6 Late upgraders (FD-4)

§5.4's guidance stands: upgrade before R_h; otherwise run `-reindex`, or
`invalidateblock` on the node's first block after R_h.

RT1 fixture B reproduces the `-reindex` path with `-b3reorghorizon=40`:

1. n6 runs without H1 and stops at 219. It then restores the wallet of n3,
   whose stake is spent but which is still a frozen V1 member, and alone
   produces its own blocks 220..265 under the frozen V1 set.
2. n6 restarts with the H1 knobs. Nothing re-judges stored blocks, and
   n0's branch would need a 46-block reorg against a horizon of 40, so the
   switch is refused (`modern-reorg-too-deep`) and n6 keeps its tip.
3. n6 restarts with `-reindex` and the H1 knobs. Its own block 220 fails
   (`bad-pos-ineligible`); n6 then syncs to n0 and agrees with every node.

The `invalidateblock` path is not exercised [H].

### 19.7 Tests

**Unit suite `finality_h1_tests`** (`src/test/finality_h1_tests.cpp`,
registered in `src/test/CMakeLists.txt`):

- Params cases:
  - (a) `h1_knobs_refused_outside_regtest`
  - (b) `h1_knobs_require_flowmesh_regtest`
  - (c) `h1_knob_values_are_checked`
  - (d) `h1_valid_regtest_configuration`
- Chain cases. They run on `H1ChainFixture`, built on
  `FinalityChainFixture` with E = 30 and R_escape = 12, which is above
  every round the fixture's own search reaches.
  - (e) `below_rh_bit_identical`: V1 byte for byte below R_h at every
    height; R_h falls inside V1 epoch 2, and a binding change between V1's
    snapshot and R_h − 1 makes the producer set differ from V1's set in
    force, so at R_h the member only V1 admits is refused and the member
    only H1 admits connects
  - (f) `broken_lineage_frozen_then_rotates`, which also checks that the
    bridge burn gate stays closed
  - (g) `carry_over_and_bootstrap_fallback`
  - (h) `rebuild_reproduces_schedule`
  - (i) `escape_restarts_schedule`
  - (j) `escape_at_activation`
  - (j′) `escape_takes_snapshot_below_floor`: an empty snapshot carries
    the set in force over; with the set in force offline and one bound,
    ACTIVE staker below `min_finality_set`, that staker's escape block
    connects; rotations keep V1's carry-over
  - (k) `rebuild_and_reload_reproduce_escape`: a disk fixture with restart
    and `-reindex-chainstate`
  - (l) `assembler_escape_round`
  - (m) `regtest_broken_pause_lifted_from_rh`
  - (m′) `regtest_handover_best_effort_under_h1`
  - (n) `h2_pin_above_rh_refuses_competitor`, with the pin parsed from the
    knob line (display order; internal order pins another identity)
- **Runtime.** About 2.5 minutes per chain case, almost all of it the
  shared `ModernPosSetup::BuildLegacy` scrypt grinding of the legacy prefix
  (`src/test/util/modern_pos_setup.h`, profiled); the chain walks add
  little. The same per-case cost explains the other finality suites' times.

**Functional RT1** is `test/functional/feature_b3_h1_producer_rotation.py`,
with two extended `test_runner.py` entries, `--fixture=B` and
`--fixture=I`.

- **Setup.** Eight local nodes: no anvil, no bridge pins and no fresh
  lineage. M = 131, R_h = 220, R_escape = 60, E = 40, minimum set 4.
- **Fixture B** (mock time):
  - Before R_h: the V1 lineage breaks and its set stays frozen; a new
    staker is excluded, and a member that spent its stake still produces.
  - Bootstrap at 220 from Snapshot(219).
  - Rotations at 260 (with the spent member's residual) and at 300 (with a
    carry-over below the minimum, while the escape set at 300 is the
    three-member snapshot itself).
  - Escape drill: n3 alone produces in round ≥ 60, and the next rotation is
    at h_e + 40.
  - A coinbase scan checks who produced where.
  - Restart, `-reindex`, and a fresh sync with the H2 pin at 300.
  - H2 failures: a wrong pin refuses to start; a wrong pin with `-reindex`
    stops at 299; the right pin with `-reindex` returns to the tip.
  - The late upgrader (§19.6).
- **Fixture I** (real time, live V1 BLS):
  - Before R_h the producer set is V1's set in force.
  - n4 stakes in exactly block 198 (mock time), so its stake is ACTIVE from
    218: in Snapshot(219) but not in V1's snapshot for its set in force at
    220. At 219 the producer set for 220 (5 members, W 115) differs from
    V1's (4 members, W 100, without n4), and n4 produces 220 alone, a block
    only the H1 schedule admits.
  - From R_h producer epochs start at 220 + 40k with no escape, while V1
    keeps certifying: finalized above 290, its epoch advanced, not broken.
  - Restart, `-reindex` and a fresh sync all agree.

**How §5.5's five items are covered.**

| Item | Where |
|---|---|
| (1) Broken lineage: frozen producers | Fixture B |
| (2) Rotations, carry-over, agreement across paths | Fixture B |
| (3) Intact lineage | Fixture I |
| (4) Escape | Fixture B's drill. Activation with no upgraded member of Snapshot(R_h − 1) is unit-only: case (j) |
| (5) Late upgrader | Fixture B |

The regtest plan's "burns stay invalid while broken" is also unit-only:
case (f).

**Regression set for this slice:**

- Every `finality_*` suite, `modern_pos_tests` and the PoS suites.
- `feature_b3_finality_mempool.py` and `feature_b3_fresh_lineage.py`.
- RT4a (`feature_b3_bridge_rt4a_round_trip.py`) and RT4
  (`feature_b3_bridge_rt4_kofn_gate.py`).

### 19.8 Implementation choices pending owner review

1. **Certificate withholding: not provided.**
   - *What the plan asks.* Regtest plan §3.6 and §7 break fixture B's V1
     lineage with `-b3regtestwithholdcertificates=131-211`: local
     block-assembly policy, never consensus, under which this node's
     assembler includes no V1 certificate in [a, b] while every signer
     stays live.
   - *Why it is absent.* A permission check refused the `CChainParams`
     accessor the assembler needs, and the `src/node/miner.cpp` hunk that
     depends on it was never applied. An earlier draft still registered,
     parsed and range-checked the knob, so it was accepted and did
     nothing. That draft was removed rather than left as a silent no-op:
     the argument parser now refuses the knob as an unknown parameter
     (unit case (c)). Wiring it needs the lead, after the owner, to apply
     the refused hunk.
   - *What RT1 fixture B does instead.* It keeps every V1 signer silent:
     the journal path is a regular file, so signing is "disabled safely".
     Its producers run with `-b3maxepochextension=1000` until 219, so the
     regtest pause cannot stop them before R_h. Restarted at 219 with the
     fixture's 40, every node derives the break at 211 from block data.
   - *What that misses.* It reproduces the frozen chain, but not "every
     signer live while broken". That drill needs the knob wired.
   - *Replay rule.* The extension differs between nodes and changes at the
     restart, against regtest plan §3.3 ("no parameter may change how a
     connected block is judged"). It is harmless only because fixture B
     never has a certificate or a burn: the extension then decides nothing
     but when V1 counts as broken and the local production pause, and
     every node agrees on the break after the restart. A wired knob would
     let fixture B use one extension throughout.
2. **The escape has no minimum-set floor.**
   - §5.1 gives the escape "its eligibility weights from Snapshot(parent)
     ... with both sets = Snapshot(parent)"; its carry-over row is written
     for the rotation. The implementation follows the escape row: it takes
     Snapshot(parent) whatever its size and carries the set otherwise in
     force over only when the snapshot is empty. Bootstrap and rotation
     keep V1's `min_finality_set` carry-over.
   - Why: the escape exists for liveness when the set in force is offline.
     With V1's floor (2 on mainnet), every in-force producer offline and
     a single bound, ACTIVE staker, the escape would re-select the offline
     set and the chain would stay halted, since no block could add stake.
     Case (j′) builds exactly that and the staker's escape block connects.
   - Cost: such an escape set can have one member. That member holds all
     bound, ACTIVE stake at the parent, so no honest staker is excluded,
     but a single key then produces until the snapshot grows (the next
     rotation keeps V1's carry-over, so it stays the only producer until
     the snapshot reaches the floor). An earlier draft kept the floor for
     escapes; the choice is the owner's.
3. **Bootstrap fallback reading.**
   - "The V1 set in force at R_h − 1" is read as the V1 set that governed
     block R_h − 1, even if V1 would have rotated at R_h.
   - ProducerSet_1 is ProducerSet_0 re-stamped 1.
   - With neither a snapshot nor a V1 set, the result is null: no
     producer, as under V1 without Set_0.
4. **Escape epoch numbering.**
   - An escape starts producer epoch k + 1 at its own height. Epochs
     therefore count rotations plus escapes, and epoch_start − R_h is a
     multiple of E only until the first escape.
   - At a rotation height the epoch advances twice (§19.2).
   - Producer epochs are numbered independently of V1 epochs, and set
     hashes carry the producer-epoch stamp.
5. **`SetInForceAt` is the single H1-aware helper.**
   - There is no separate producer-set helper. From R_h, `SetInForceAt`
     means the producer set, no longer V1's set.
   - Every caller is audited in §19.3. A future caller that wants V1's set
     at h ≥ R_h must read `Projected().current`.
6. **The knobs need `-b3flowmeshtest`.** This is not a deviation: regtest
   plan §3.3 says its startup knobs and the pin file "Both need
   `-b3modernregtest -b3flowmeshtest`", as `-b3bridgepins` and the
   fresh-lineage knobs already do.
7. **R_escape bound.** The range is [1, 99,999], not the planned 100,000:
   the assembler's round search tries only rounds below 100,000, so a
   bound of 100,000 could never be produced locally.
8. **The regtest pause is lifted in the assembler.** The regtest plan says
   H1 switches `PlanRegtestFinalityProduction` to the producer schedule.
   Instead, the override lives in `CreateNewBlock` (§19.3), and the policy
   header is unchanged. From R_h it lifts both pauses: LINEAGE_BROKEN and
   REQUIRE_HANDOVER (best effort: a pooled certificate is still included).
   An earlier draft kept REQUIRE_HANDOVER, which would stop every regtest
   staking loop for good at the last carrier whenever an intact lineage
   lost its quorum after R_h, even past R_escape (case (m′)).
9. **Escape weight and wallet text (follow-up, outside this slice).**
   - H1-R5 asks the wallet to report escape status. It does not yet:
     `getstakinginfo.active_weight` is the normal-round weight, so a
     validator that is only in the escape set shows 0 there while its
     staking loop runs and waits for escape rounds. The escape status is
     reported only in `getfinalitystatus.producers` (`escape_set`,
     `escape_time`).
   - The fix needs `interfaces::StakingStatus` and the wallet RPC
     (`src/wallet/rpc/staking.cpp`), both outside this slice: report the
     escape-round weight from the `ModernEligibilityWeights(key, tip,
     R_escape)` call the staking loop already makes, and R_escape or the
     escape time. The `getstakinginfo` help text there still says newly
     ACTIVE stake enters "only through a later certified rotation", which
     is wrong from R_h.
   - The Qt page's finality-membership fields stay V1's (H1-R3), while its
     eligible weight follows H1.
10. **Escape races and stale parents, unmodelled.**
    - An escape block cannot come early (exact timestamps). Its race with a
      late in-force block on the same parent, and the fork choice between
      them, are still for the §13 model check (H-H1) [H]. No test builds
      such a competing pair.
    - Escape status depends only on a block's timestamp relative to its
      parent. So any block built now on a parent older than R_escape ×
      round seconds is an escape block weighted by Snapshot(parent). On
      mainnet that is 24 h, about 1,100 blocks at 78 s, inside the
      1,440-block reorg horizon. Such a stale-parent fork can be signed by
      every bound, ACTIVE staker at that parent, including keys whose stake
      was later spent and sold, not only by the producers in force, and it
      restarts the schedule. Equal per-block work and the future-time bound
      should keep it from overtaking the honest chain, but nothing checks
      that. Before any mainnet R_h ruling the §13 model check should add an
      escape on a parent deeper than R_escape, with its interaction with
      the reorg horizon and the H2 pin [H].
11. **How RT1 differs from the regtest plan.**
    - RT1 runs without bridge pins and without the fresh lineage.
    - RT1's fixture I adds `-b3h1height=220` to the plan's fixture I. That
      fixture's table row says no H1, and RT3, RT4 and RT6 still run it
      without H1.

### 19.9 What stays open

- **FD-4**: R_h, the adoption window and the exchange notice are the
  owner's, after O2. No shipped network sets R_h.
- **The mainnet H2 entry**: a release step (§19.5).
- **Governing text** (§16): `doc/design/b3-cross-chain-finality-v1.md:172-173`
  still describes handover-gated production and one stake universe.
- **The §13 model check** of H-H1, including the stale-parent escape
  (§19.8 item 10).
- **The withholding knob** (§19.8 item 1) and **wallet escape status**
  (§19.8 item 9): both need hunks outside this slice.
- **The `invalidateblock` path** for late upgraders.
- **Unchanged by H1**: an unstaked member's residual of up to 2E blocks
  (§5.4).

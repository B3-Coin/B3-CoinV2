# B3 Bridge V2: staker-signed fast deposits from Arbitrum

> **PROPOSAL, no governing authority.** 2026-10-03, revision 2 (review
> fixes), base `44acff3`, branch `feat/flowmeshV2-dev-claude`. Answers the
> owner's 2026-10-03 messages (relayed, not in the repo): "cant we have our
> own signer", "3 of 4 or 2of3 signatures", "signers can be rotated" and "to
> make it fully decentralise d". Nothing built, run or measured; no node or
> chain contacted. It overrides OD-8, so it needs an explicit owner ruling
> (B-25).

**Labels.** [E] verified; [I] design choice; [H] estimate. Repo facts are
`path:line`; external facts are URLs. Numbers and IDs come from the
[architecture](b3-bridge-v2-arbitrum-proposal.md),
[contracts spec](b3-bridge-v2-contracts-spec.md),
[node changes](b3-bridge-v2-node-changes.md),
[finality dependency](b3-bridge-v2-finality-dependency.md) and
[regtest plan](b3-bridge-v2-regtest-plan.md).

---

## 0. Summary

- **Yes, B3 can have its own signers, but not a small hand-picked set.** They
  are B3's stakers: the stake-ranked committee of up to 64 keys that V2 is
  designed to use for withdrawals (B-22). Consensus computes it every epoch;
  each handover still needs the outgoing quorum's signature (§2 row 3).
- **New: staker-signed fast deposits (FSD).** Once the Arbitrum batch
  carrying a Vault_A deposit is final on Ethereum, the committee signs and B3
  mints bUSD in about 16-29 minutes [H], instead of ~6.4 days or an LP fee.
- **Bounded and self-checking.** Unsettled FSD mints stay below the smaller
  of a cap (default 10,000 bUSD) and a funded first-loss pool. While Arbitrum
  confirms, the trustless Outbox proof (kind 8) settles or refutes each FSD
  within about 7.5-16 days plus grace. A refutation freezes the lane and
  burns the false amount from the pool; the real depositor is paid once.
- **No new contract. Mainnet only after V2 P3, FSD bonds, a funded pool and
  a measured stake distribution**; regtest milestone RT7b.
- **Honest limits.** "Fully decentralized" is not reachable (§1). No B3-side
  key can mint bUSD or move vault funds, but owner-coordinated hard forks,
  deployer pins, the owner relayer and an owner-funded treasury remain, and
  the committee is only as independent as B3's unmeasured stake distribution
  (O2).

---

## 1. What "fully decentralized" can and cannot mean

The owner's stance is "as decentralized as possible", not 100% (2026-08-23,
memory only). Goal: **no person's or company's key can mint, move, pause,
upgrade or release funds.** These powers remain:

| Held outside B3 | Power | Source |
|---|---|---|
| Tether; USDT0's 3-of-5 Safe | Freeze, seize or re-point either vault's token | architecture :474-475 [E] |
| LayerZero DVNs | Forge unbacked USDT0 | architecture :476 [E/H] |
| Arbitrum Security Council; sequencer | Force-confirm or upgrade: false mints, settlements or refutations; skew timestamps | architecture :288-291, :477 [E] |
| Ethereum sync committee | False anchors behind kinds 7, 8, 14 | architecture :481 [E] |
| Ethereum forks | A B3 hard fork each (~yearly) | node changes :711-725 |

| Held inside B3 | Power | Source |
|---|---|---|
| Owner-coordinated hard forks | Re-pin; migrate a reserve; lift a HARD freeze; void a delayed penalty | node changes §7.6, §12.3 |
| Bootstrap | Deployer pins under public review; ≤ 5 owner-chosen watchers may cancel before opening | contracts spec :201; finality :583 |
| Owner relayer | Default liveness; anyone may relay; 30 days with none strands | architecture :500 |
| Treasury | One P2PKH key (`src/kernel/chainparams.cpp:289-290`); funds the LP backstop (N-2) and likely the pool | node changes :801 |
| Stake concentration | Two-thirds of committee weight controls it; a third halts it | §3; O2 |

---

## 2. Decentralization checklist

| # | Requirement | How the design meets it | Status |
|---|---|---|---|
| 1 | Chosen by stake | Weight ≥ W/128, top 64, ties by key (finality :521-526); 64 fits Ethereum's gas cap (architecture :179-187) | Designed (B-22) |
| 2 | Open entry, anti-Sybil | 333 B3 per key (`src/kernel/chainparams.cpp:274-277`). Splitting below the floor buys nothing; above it, it buys headcount: ~13% of weight can block quorum (finality :523-526) | B-22 raises the cost only |
| 3 | Rotation | Each 1,440-block epoch (`src/kernel/chainparams.cpp:292`) consensus computes the next committee from stake. The outgoing dual quorum must certify the handover; there is no escape (H1's is for producers, finality :37-39). A silent committee keeps B3 authority 33-43 days (finality :296-297), cannot be replaced on the contracts, and strands the vaults after 30 days (architecture :441-447) | Needs P3; replacement path unruled (finality :529-532, FD-15) |
| 4 | Time-bounded handover | Contracts pin the successor the outgoing two-thirds signs (contracts spec :235, :241-243); honest members sign only B3's set. Rotations ≥ 6 h apart; deposits close 72 h without one; expiry 30 days | A captured quorum can pin a fake set (§3) |
| 5 | No admin or upgrade key | None (contracts spec :45-46); B3 changes only by hard fork | Watchers cancel only in review |
| 6 | Permissionless relayers | Anyone submits certificates, records, FSDs, refutations (architecture :500) | Designed |
| 7 | Penalties | FSD bonds (§4.9); stake leaves by an ordinary spend today (`src/modern/stake.h:33-37`) | **Missing**: V2-D4 "None" (finality :476), B-30 |
| 8 | Automatic freeze | Only consensus evidence (§4.6) | Unfreeze by hard fork (B-29) |
| 9 | Reproducible, audited | Guix (`contrib/guix/`); two builds match code hashes (S7); audits (S6) | Unscheduled, no cost |
| 10 | LP optional | ~6.4 days, or 16-29 min by FSD | Designed |
| 11 | Independent signers | Own Arbitrum and Ethereum nodes | Policy; Nitro is in practice the only Arbitrum One client [H] |

---

## 3. Why not a small rotating 3-of-4 or 2-of-3 set

**Capture over time** [I computed, binomial; each seat goes to a 20%-stake
attacker with probability 0.2]:

| Set | Capture per rotation | Over 100 rotations |
|---|---|---|
| 3-of-4 (also a 4-key committee) | 2.72% | 93.7% |
| 2-of-3 | 10.4% | ~100% |
| 23 of 34 (Set_2's size) | 2.3 × 10⁻⁹ | 2.3 × 10⁻⁷ |
| 43 of 64 | 3.8 × 10⁻¹⁶ | 3.8 × 10⁻¹⁴ |

The table shows only why size matters. The committee is stake-ranked, not
sampled, so **decentralization equals stake distribution**: two-thirds of
weight, with enough keys, controls it, and it may have only 4 keys
(contracts spec :199). Until O2 shows that no few keys, the owner's
included, hold a third of weight, it is not independent of them (B-36); O2
sees keys, not owners, so this is a lower bound on concentration.

**Self-chosen successors.** If signers pick their successors, as multisig
owners do, one capture is permanent. B3 computes successors from stake, so
FSD minting cannot be captured for good: a fake-successor certificate
becomes kind 13 evidence and freezes FSD (node changes §8.5). **The contracts
differ.** They pin whatever successor the outgoing two-thirds signs;
rotated-out sets cannot halt (contracts spec :249-253); one certificate
serves both chains (:122-124). One two-thirds capture of any epoch thus
controls both vaults permanently, and kind 13 closes all burns. Only
committee size, stake weighting, P3, bonds and the unruled replacement path
(finality :529-532) protect against it.

**Same quorum, little new power.** Such a coalition can already drain a vault
(architecture :478). FSD adds at most its cap, plus honest failures (shared
Nitro bug, Security Council action) that the cap, pool and freeze bound.

---

## 4. The fast deposit (FSD)

### 4.1 The attestation

An FSD is a kind 8 mint whose Outbox Merkle path is replaced by a committee
signature plus a proof that the Arbitrum batch is final on Ethereum [I]:

```
fsdDigest = TaggedHash("B3/BRIDGE/FSD/V1",
    chainDomain ‖ lineageId ‖ u64 epoch ‖ content)
content = vaultEntryId ‖ u64 depositId ‖ u256 amount ‖ bytes32 b3Recipient ‖
    u32 maxLpFeePpm ‖ u64 outboxPosition ‖ bytes32 outboxItem ‖ u64 l2Block ‖
    u64 batchSeq ‖ bytes32 batchAcc ‖ u64 l1BatchBlock
```

Sources: the Deposit message (contracts spec §6.3), the `L2ToL1Tx` event
(node changes §5.3;
https://raw.githubusercontent.com/OffchainLabs/nitro-precompile-interfaces/main/ArbSys.sol
[E]) and the Bridge's `sequencerInboxAccs`
(https://raw.githubusercontent.com/OffchainLabs/nitro-contracts/main/src/bridge/AbsBridge.sol
[E]). The tag keeps FSD signatures apart from other messages.
**Equivocation** is one key signing different `content` for one
(vaultEntryId, depositId) under one `chainDomain`; re-signing identical
content under a later epoch is not.

### 4.2 When a signer signs

Only when its **own** nodes show [I]: the deposit's block at or below
Arbitrum's `finalized` tag, so its batch is in a finalized Ethereum block
(https://docs.arbitrum.io/how-arbitrum-works/reference/finality-and-reorgs)
[E]; every event field and `batchSeq` matching; the lineage healthy, itself
in the committee, the lane OPEN with headroom; and no different content in
its journal. Partial signatures travel over B3's P2P like finality votes;
anyone aggregates and submits.

### 4.3 How B3 validates it (new kind 14, FSD_MINT)

Body: the statement, the item preimage (kind 8's fields without the path),
the signing set header, bitmap, aggregate signature, and EIP-1186 proofs of
the Bridge and Outbox accounts against a retained anchor; about 10-13 KB [H,
O10, O12]. Valid only if [I]:

1. The activation height is reached and the lane is OPEN (§4.6).
2. Kind 8 checks 2-5 pass on the preimage (node changes :266-278); the
   nullifier (42161, Vault_A, depositId) is unused; no ATTESTED key or
   proven Vault_A message has this `outboxPosition`.
3. **Time binding:** `l2_timestamp` lies within [anchor time − 48 h, anchor
   time + 768 s].
4. `epoch` is current, or the previous one within 90 blocks of the handover;
   gates C2 and C3 hold (node changes :523-524).
5. The aggregate verifies over `fsdDigest` with ⌊2n/3⌋+1 members **and**
   ⌊2W/3⌋+1 weight of that committee (contracts spec §3.6).
6. The anchor is Ethereum-finalized; `sequencerInboxAccs[batchSeq] ==
   batchAcc`; the Bridge's code hash and implementation, and the Outbox
   slots kind 7 checks (implementation, slots 0 and 1), equal the pins; a new
   kind 7 root was accepted within 2,880 blocks. FSD thus stops once
   settlement becomes impossible; only "this batch contains this deposit"
   rests on the committee.
7. The mint queue is empty and block, epoch and FSD caps (§4.7) have
   headroom, counting earlier records in the block. FSDs never queue.

### 4.4 Key states and accounting

| State | Entered by | Effect |
|---|---|---|
| ATTESTED | Kind 14 | Nullifier consumed; payee paid (§4.8); `fsd_outstanding_A` += a; caps used |
| SETTLED | Kind 8 with the attested (depositId, amount, recipient, `fee_eff`) | Mints nothing (`output_index` 0xFFFFFFFF, no caps or queue); `fsd_outstanding_A` −= a; `reserve_A` += a. A differing item or position also HARD-freezes, with no false mint or penalty |
| REFUTED | Kind 15(b) or (c), the coverage deadline (§4.5), or a kind 8 with different economic fields | `fsd_outstanding_A` −= a; `false_A` += a; the pool burns up to a, once, reducing `false_A`; HARD freeze; penalty evidence |
| PAID | The key's kind 8 after REFUTED | Fields differ: mints the proven tuple normally (payee, caps, queue). Fields equal: no mint to the payee; `reserve_A` += a; re-credits the pool's burn and clears `false_A` |

A mismatching kind 8 on an ATTESTED key applies REFUTED and PAID together. A
kind 8 on a consumed nullifier is valid only for an ATTESTED or REFUTED key,
once (node changes §5.3 checks 5-6 must say so). The invariant becomes supply
≤ Σ reserve_v + `fsd_outstanding_A` + `false_A` (false mints not yet burned).

### 4.5 Refutation and deadlines

**Order facts [E].** Each Vault_A deposit takes the next deposit id and the
next global Outbox position (contracts spec §6.2-§6.3;
https://github.com/OffchainLabs/nitro/blob/master/precompiles/ArbSys.go),
and Nitro never lowers a block timestamp (`createNewHeader`,
https://github.com/OffchainLabs/nitro/blob/master/arbos/block_processor.go).
Ids, positions and timestamps rise together.

**Kind 15, FSD_FAULT** (anyone):

- (a) Equivocation (§4.1): slashes that key's bond and bars it from FSD; it
  HARD-freezes only when both contents carry a dual quorum.
- (b) An Outbox path under a confirmed `sendRoot` placing a different item
  at the attested position; an accepted kind 8 or 11 there does the same
  automatically.
- (c) A proven Vault_A message (d′, p′, ts′) under a confirmed root breaking
  the order against the attested (d, p, ts): p′ < p with ts′ > ts or d′ > d,
  or p′ > p with ts′ < ts or d′ < d (ids for deposits only).

**Coverage deadline.** B3 records the highest proven Vault_A position (kinds
8 and 11). Once it passes p, a confirmed root covers p and anyone can submit
the kind 8 or a 15(b); a key still ATTESTED `fsd_settle_grace` blocks later
(default 2,880) is REFUTED. By the time binding, the first daily heartbeat
after ts (node changes §8.3) covers p or refutes by 15(c), so every key
resolves about 7.5-16 days after its deposit, plus the grace.

**Timeout.** A key not passed by any proven position after
`fsd_reconcile_blocks` (default 30,240: 21 days at 60 s, ~26 at 78 s) makes
the lane SOFT: confirmations or heartbeats have stopped, which also closes
Vault_A's gate (A2), and the next proven heartbeat resolves the key.

### 4.6 Freeze

| State | Trigger | New FSDs | Ends |
|---|---|---|---|
| OPEN | — | Yes | — |
| SOFT | Timeout; Vault_A impaired or gate closed; finality lag > 90; a pending Vault_A migration (§4.7) | No | By itself once Arbitrum confirms and the cause clears |
| HARD | A REFUTED key; metadata mismatch; quorum equivocation; kind 13 | No | **Only a coordinated hard fork** with a new FSD tag (B-29) |

Only consensus evidence freezes; no single key, owner or vote can freeze or
unfreeze. Settlements and refutations always continue.

### 4.7 Cap and first-loss pool

**Pool [I]:** keyless bUSD in consensus state; anyone may contribute. No LP
float exists to seed it (architecture :365), so FSD opens only after someone,
in practice the owner's single-key treasury, funds it. Contributors bear
losses pro rata. **Withdrawals** complete after `fsd_reconcile_blocks`
notice, only if the remaining pool covers `fsd_outstanding_A` + `false_A`; a
HARD freeze locks them until the burn is applied.

**Cap rule:** `fsd_outstanding_A + amount ≤ min(FSD_CAP_A, pool − pending
withdrawals)`. It caps what is unsettled, not flow, so it holds through
~14.7-day disputes; normally it acts per ~6.5-day window (five epochs of
Vault_A caps [computed]). Above it: an LP, the treasury LP (N-2), or the
~6.4-day path.

**Migration.** Outstanding FSD amounts are not in `reserve_A`, so a Vault_A
migration leaf (node changes §7.6) would leave their tokens behind: the
migrating hard fork closes the lane (SOFT) and its leaf waits until
`fsd_outstanding_A` is zero.

### 4.8 LPs and exactly-once

Each key pays its proven economic tuple exactly once, by an FSD that
settles or by a kind 8; either pays an LP that filled first. A refuted FSD
is a pool-covered loss, never a second payment.

- **LP before FSD:** the FSD resolves the payee as kind 8 does (node changes
  §6), repaying the LP in ~16-29 min.
- **LP after FSD:** fills for an ATTESTED key, or with an FSD in the same
  block, are invalid (node changes §6 rules 1, 5, extended).
- **Buckets stay until SETTLED or PAID**, so after a refutation the kind 8
  still pays an LP that filled the real tuple.
- **Wallets** keep B-5's 30 bps default, as `maxLpFeePpm` is fixed at
  deposit; they offer 0 only with a warning, when B3 shows headroom for the
  amount plus pending FSDs and an empty queue.

### 4.9 Penalties FSD needs (mainnet gate)

- **FSD bond [I]:** signing needs a separate bonded output outside committee
  weight, so a slash never changes membership or stalls finality. It stays
  locked until every FSD the key signed resolves, and for
  `fsd_reconcile_blocks` after it leaves the committee.
- **Automatic, B3-provable:** 15(a), and kind 13 under P3 only (V1 voting can
  honestly export an orphan, finality :57-59): 100% of the bond.
- **Arbitrum-dependent** (REFUTED keys): B3 cannot tell a lie from a Security
  Council rewrite or a Nitro bug all signers share, so each attesting key
  forfeits a bounded share [10%] of its bond after a 30-day delay; an equal
  kind 8 cancels it, and a coordinated hard fork may void it. Stake weight is
  never slashed.
- **Proceeds** are B3, not bUSD, so they are burned: deterrence, not cover.
- **Condition:** a dual quorum's bonds should be worth over 3 × `FSD_CAP_A`;
  B3 has no price feed, so this is a ruling, not a rule.
- **Reward:** nothing rewards signing (finality :536-541); FD-16.

### 4.10 Latency

**[H]** Batch posted 0-3 min, Ethereum finality 13-19, signing 0.5-2,
light-client update then FSD 2-4 blocks (2.6-5.2 min, architecture :314):
**16-29 min**, against route (c)'s 15-26 with an LP (architecture :342);
FlowMesh V1 adds 40-42. Assumes full participation and ~78 s blocks (O8). A
B3 reorg undoes an FSD like any mint.

---

## 5. Contract side

Nothing new: Vault_A V2 already emits one `sendTxToL1` per deposit (contracts
spec §6.2 step 7), carrying all an FSD and its proof need. Withdrawals are
unchanged; no contract reads an FSD.

---

## 6. Risks and bounds

| Risk | Bound or answer |
|---|---|
| Worst-case loss | The effective cap; the first refutation HARD-freezes the lane |
| Who absorbs it | Pool contributors, then every bUSD holder, first come first served (architecture §12 row 14) |
| Detection delay | ~7.5-16 days plus grace while Arbitrum confirms; longer only keeps the lane SOFT |
| Arbitrum governance, Nitro bug | A force-confirmation could settle a false FSD, a rewrite or shared bug refute honest ones; B3 cannot tell [I]. Cap, pool, freeze and bounded penalties outside committee weight keep finality safe. One client is a shared-bug risk no policy removes |
| Shared RPC; sync committee | Can deceive honest signers or anchors; loss stays within the cap |
| Liveness | A silent third halts the whole bridge (architecture :559), FSD included; deposits still mint later by kind 8 or LP |
| Disclosure | Replace "minted only from Ethereum light-client proofs" (architecture :486-488) with "minted from Ethereum light-client proofs or, for Arbitrum deposits up to a cap, from B3 stakers' signatures those proofs later check; a false attestation is a loss up to that cap, borne first by a first-loss pool" |

---

## 7. Stage placement and effort

- **Mainnet: new stage S10, FSD**, after S9 (kinds 7 and 8 seen live), a
  **penalties stage** (FSD bonds, V2-D4 changed) after S5 (P3), a funded pool
  and B-36's stake gate. Its hard fork pins `FSD_CAP_A`, the Bridge and
  Outbox pins and the activation height.
- **Regtest: RT7b**, `feature_b3_bridge_rt7b_fsd.py`, after RT7, under the
  staging signer's committee, with a mock Bridge on anvil 31337 and a
  `finalized` view of 31338. Tests cover every §4.4 transition (once only),
  each kind 15 case, both deadlines, pool limits, LPs, re-signing, every §4.3
  refusal, reorg, `-reindex` and FlowMesh V1 credit. RT8 re-runs it under
  P3; RT9 adds a refutation drill.
- **Effort [H]**, one engineer: node rules 1.2-1.8k lines; storage proofs
  0.3-0.5k; signer service 1-1.5k; RPC and wallet 0.3-0.5k; tests 1.8-2.4k:
  **8-12 person-weeks** to RT7b. Bonds and slashing 5-10 more, plus 2-4
  weeks of node audit; no audit cost figure.

---

## 8. Owner decisions

Defaults in brackets; the handoff forbids inventing economic rules
(`doc/design/b3-master-handoff.md:1160`), so every number needs a ruling.

| ID | Decision [default] |
|---|---|
| **B-25** | **Bounded OD-8 exception:** committee attestations may mint Arbitrum deposits within the effective cap, always settled or refuted by Outbox proofs [**explicit ruling needed**; reverses "OD-8's signer-set ban stands" (README :67) and architecture :705] |
| B-26 | Signers = the B-22 committee; no separate 3-of-4 or 2-of-3 set [yes; accepts the contracts' successor-capture risk, §3] |
| B-27 | Effective cap min(`FSD_CAP_A`, free pool) on unsettled amounts [10,000 bUSD pilot; 50,000 after audit] |
| B-28 | Keyless first-loss pool; withdrawals bounded by outstanding amounts; FSD opens once it is funded, likely by the treasury [yes] |
| B-29 | Freeze [HARD on refutation, metadata mismatch, quorum equivocation or kind 13, lifted only by hard fork with a new tag; SOFT otherwise, self-clearing] |
| B-30 | Penalties before mainnet FSD [separate bond held until its FSDs resolve; 100% for 15(a) and, under P3, kind 13; 10% after a voidable 30-day delay for refutations; stake weight never slashed; changes V2-D4/FD-13] |
| B-31 | `fsd_reconcile_blocks` [30,240]; `fsd_settle_grace` [2,880]; time window [−48 h/+768 s]; kind 7 freshness [2,880]; epoch grace [90] |
| B-32 | Batch finality and Outbox pins checked in consensus [yes] |
| B-33 | No FSD fee; wallet `maxLpFeePpm` [30 bps; 0 only with shown headroom] |
| B-34 | Signers run their own Arbitrum and Ethereum nodes [policy]; reward under FD-16 |
| B-35 | S10 after S9, penalties and a funded pool; RT7b after RT7 [yes] |
| B-36 | S10 stake gate [O2 shows the top 3 keys, and separately the owner's keys, below a third of committee weight] |

---

## 9. Conflicts and open items

**Reconcile before any FSD code** (adds to architecture §15): OD-8,
`doc/design/b3-open-decisions.md:306`; architecture :35, :37-38,
:69-71, :286, :478, :486-488 (replace, §6), :664, :705; README :11, :15, :67,
:98; node changes :69-71, §5.3 checks 5-6, §7.6; finality :476 and :512 ("no
slashing"); master handoff :285 and :956 (committees and slashing are future
scope); `CLAUDE.md:12-13` and `:87-88` (rule 3: bonds change Modern PoS),
within the one consensus override.

**Open:** Bridge and Outbox slots and pins [H, O10]; proof sizes [H, O12];
whether Nitro serves `finalized` as documented [H]; B3's price for the bond
condition; whether kind 7 freshness pauses FSD too often during disputes;
late plain kind 8s after a migration also leave tokens behind (node
changes); no delegation for small stakers.

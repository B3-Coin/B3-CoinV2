# B3 Bridge V2: staker-signed fast deposits from Arbitrum (later option)

> **PROPOSAL, no governing authority; a LATER OPTION, not part of launch.**
> Revision 3, 2026-10-04, base `1d1d6da`, branch `feat/flowmeshV2-dev-claude`.
> On 2026-10-03 the owner ruled for the simpler launch plan without an LP
> (relayed): "ok then I think simpler plan , and its without
> LP ? I have no issue from LP, but just doesnt know why we need it". Launch
> has one Ethereum vault and no Arbitrum vault, LP or signer-attested mint
> (architecture §0). FSD attests deposits into an Arbitrum vault (Vault_A),
> so it needs the **later Arbitrum vault stage** (architecture §17 item 1)
> first. B-25 to B-36 are deferred; **B-25, an OD-8 exception, still needs
> an explicit owner ruling** if FSD is ever pursued. Nothing built, run or
> measured; no node or chain contacted.

**Labels.** [E] verified; [I] design choice; [H] estimate. Untagged
citations of the [launch plan](b3-bridge-v2-arbitrum-proposal.md)
("architecture"), [contracts spec](b3-bridge-v2-contracts-spec.md),
[node changes](b3-bridge-v2-node-changes.md),
[finality](b3-bridge-v2-finality-dependency.md) and README are current
sections. "r8" (the Arbitrum vault design), "r8 contracts spec" and "r10
node changes" are those files at `1d1d6da`; their Arbitrum parts now sit in
contracts spec Appendix B.1 and node changes §17.

---

## 0. Summary

- **Yes, B3 can have its own signers, but not a small hand-picked set.** They
  are B3's stakers: the stake-ranked committee of up to 64 keys V2 uses for
  withdrawals (B-22, awaiting explicit owner approval, architecture §6).
  Consensus recomputes it every epoch; the outgoing quorum signs each
  handover (§2 row 3).
- **Later option only.** With a Vault_A, once the Arbitrum batch carrying a
  deposit is final on Ethereum, the committee signs and B3 mints bUSD in
  ~16-29 minutes [H], not ~6.4-6.5 days or an LP fee. Launch route (b)
  already brings Arbitrum users to the Ethereum vault in 17-28 minutes [H]
  with no signer or LP (architecture §8.2).
- **Bounded and self-checking.** Unsettled FSD mints stay below a cap
  (default 10,000 bUSD) and a funded first-loss pool. The Outbox proof (kind
  8) settles or refutes each FSD within ~7.5-16 days plus grace; a
  refutation freezes the lane and the pool burns the false amount.
- **No contract beyond the Arbitrum vault stage's. Mainnet only after that
  stage, V2 P3, FSD bonds, a funded pool and a measured stake
  distribution**; regtest RT7b after RT7 returns with it.
- **Honest limits** (§1): owner-coordinated hard forks, deployer pins, the
  owner relayer and treasury remain; the committee is as independent as
  B3's unmeasured stake distribution (O2).

---

## 1. What "fully decentralized" can and cannot mean

The owner's stance is "as decentralized as possible", not 100% (2026-08-23,
memory only). Goal: **no person's or company's key can mint, move, pause,
upgrade or release funds.** Outside B3, Tether, USDT0's 3-of-5 Safe,
LayerZero DVNs, Arbitrum's Security Council and sequencer, the Ethereum
sync committee and Ethereum forks keep their powers over a later Vault_A
(r8 :288-291, :474-481; at launch, architecture §10). Inside B3:

| Held inside B3 | Power | Source |
|---|---|---|
| Owner-coordinated hard forks | Re-pin; migrate a reserve; lift a HARD freeze; void a delayed penalty | node changes §7.6, §12.3 |
| Bootstrap | Deployer pins and a one-time 3-of-4 attestation under public review; ≤ 5 owner-chosen watchers may cancel before opening | contracts spec §3.1, §5; finality §11 |
| Owner relayer | Default liveness; anyone may relay; 30 days with none strands | architecture §11 |
| Treasury | One P2PKH key (`src/kernel/chainparams.cpp:289-290`); likely funds the pool, and N-2's LP if revived | node changes §16 |
| Stake concentration | Two-thirds of committee weight controls it; a third halts it | §3; O2 |

---

## 2. Decentralization checklist

| # | Requirement | How the design meets it | Status |
|---|---|---|---|
| 1 | Chosen by stake | Weight ≥ W/128, top 64, ties by key (finality §10.3); 64 fits Ethereum's gas cap (architecture §4.2) | B-22, awaiting explicit owner approval |
| 2 | Open entry, anti-Sybil | 333 B3 per key (`src/kernel/chainparams.cpp:274-277`). Splitting below the floor buys nothing; above it, it buys headcount: ~13% of weight can block quorum (finality §10.3) | B-22 raises the cost only |
| 3 | Rotation | Consensus recomputes the committee from stake each 1,440-block epoch (`src/kernel/chainparams.cpp:292`); the outgoing dual quorum must certify the handover, with no escape (H1's is for producers, finality §0 item 1). A silent committee keeps B3 authority 33-43 days (finality §6.3), cannot be replaced on the contracts, and strands every vault after 30 days (architecture §9.1) | Needs P3; replacement path unruled (finality §10.3, FD-15) |
| 4 | Time-bounded handover | Contracts pin the successor the outgoing two-thirds signs (contracts spec §3.3 steps 6, 10); honest members sign only B3's set. Rotations ≥ 6 h apart; deposits close 72 h without one; expiry 30 days (B-10) | A captured quorum can pin a fake set (§3) |
| 5 | No admin or upgrade key | None (contracts spec §0); B3 changes only by hard fork | Watchers cancel only in review |
| 6 | Permissionless relayers | Anyone submits certificates, records, FSDs, refutations (r8 :500) | Designed |
| 7 | Penalties | FSD bonds (§4.9); stake leaves by an ordinary spend today (`src/modern/stake.h:33-37`) | **Missing**: V2-D4 "None" (finality §9.4), B-30 |
| 8 | Automatic freeze | Only consensus evidence (§4.6) | Unfreeze by hard fork (B-29) |
| 9 | Reproducible, audited | Guix (`contrib/guix/`); two builds match code hashes (S7); audits (S6) | Unscheduled |
| 10 | LP optional | None at launch (architecture §18); with Vault_A, ~6.4-6.5 days, an optional LP, or FSD | Later option |
| 11 | Independent signers | Own Arbitrum and Ethereum nodes | Policy; Nitro is effectively the only client [H] |

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

A 2-of-3 set also tolerates no Byzantine signer (Codex review; architecture
§18). The committee is stake-ranked, not sampled, so **decentralization equals stake distribution**: two-thirds
of weight, with enough keys, controls it, and it may have only 4 keys
(contracts spec §3.1). Until O2 shows that no few keys, the owner's
included, hold a third of weight, it is not independent of them (B-36); O2
sees keys, not owners: a lower bound on concentration.

**Self-chosen successors.** If signers pick their successors, as multisig
owners do, one capture is permanent. B3 computes successors from stake, so a
fake-successor certificate becomes kind 13 evidence and freezes FSD (node
changes §8.5). **The contracts differ:** they pin whatever successor the
outgoing two-thirds signs; rotated-out sets cannot halt (contracts spec
§3.4). With a later Vault_A one certificate serves both chains (contracts
spec §2.2), so one capture of any epoch controls both vaults, and kind 13
closes all burns. Only committee size, stake weighting, P3, bonds and the
unruled replacement path (finality §10.3) protect against it.

**Same quorum, little new power.** Such a coalition can already drain vaults
(architecture §10; r8 :478). FSD adds at most its cap, plus honest failures
(Nitro bug, Security Council action) within cap, pool and freeze.

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

Sources: Vault_A's Deposit message (r8 contracts spec §6.3), the
`L2ToL1Tx` event (kind 8's fields, r10 node changes §5.3;
https://raw.githubusercontent.com/OffchainLabs/nitro-precompile-interfaces/main/ArbSys.sol
[E]) and the Bridge's `sequencerInboxAccs`
(https://raw.githubusercontent.com/OffchainLabs/nitro-contracts/main/src/bridge/AbsBridge.sol
[E]). **Equivocation** is one key signing different `content` for one
(vaultEntryId, depositId) under one `chainDomain`; re-signing identical
content under a later epoch is not.

### 4.2 When a signer signs

Only when its **own** nodes show [I]: the deposit's block at or below
Arbitrum's `finalized` tag, so its batch is in a finalized Ethereum block
(https://docs.arbitrum.io/how-arbitrum-works/reference/finality-and-reorgs)
[E]; every event field and `batchSeq` matching; the lineage healthy, itself
in the committee, the lane OPEN with headroom; and no different content in
its journal. Partial signatures travel over B3's P2P; anyone aggregates and
submits.

### 4.3 How B3 validates it (new kind 14, FSD_MINT)

Body: the statement, the item preimage (kind 8's fields without the path),
the signing set header, bitmap, aggregate signature, and EIP-1186 proofs of
the Bridge and Outbox accounts against a retained anchor; ~10-13 KB [H,
O10, O12]. Valid only if [I]:

1. The activation height is reached and the lane is OPEN (§4.6).
2. Kind 8 checks 2-5 pass on the preimage (r10 node changes §5.3); the
   nullifier (42161, Vault_A, depositId) is unused; no ATTESTED key or
   proven Vault_A message has this `outboxPosition`.
3. **Time binding:** `l2_timestamp` lies within [anchor time − 48 h, anchor
   time + 768 s].
4. `epoch` is current, or the previous one within 90 blocks of the handover;
   gates C2 and C3 hold (node changes §8.4).
5. The aggregate verifies over `fsdDigest` with ⌊2n/3⌋+1 members **and**
   ⌊2W/3⌋+1 weight of that committee (contracts spec §3.6).
6. The anchor is Ethereum-finalized; `sequencerInboxAccs[batchSeq] ==
   batchAcc`; the Bridge's code hash and implementation, and the Outbox
   slots kind 7 checks (implementation, slots 0 and 1), equal the pins; a new
   kind 7 root was accepted within 2,880 blocks. FSD thus stops once
   settlement becomes impossible; only "this batch contains this deposit"
   rests on the committee.
7. If the LP option (node changes §17 item 3) has returned, the mint queue
   is empty; block, epoch and FSD caps (§4.7) have headroom, counting
   earlier records in the block. FSDs never queue.

### 4.4 Key states and accounting

| State | Entered by | Effect |
|---|---|---|
| ATTESTED | Kind 14 | Nullifier consumed; recipient paid (a revived LP's payee instead, §4.8); `fsd_outstanding_A` += a; caps used |
| SETTLED | Kind 8 with the attested (depositId, amount, recipient, `fee_eff`) | Mints nothing (`output_index` 0xFFFFFFFF, no caps or queue); `fsd_outstanding_A` −= a; `reserve_A` += a. A differing item or position also HARD-freezes, with no false mint or penalty |
| REFUTED | Kind 15(b) or (c), the coverage deadline (§4.5), or a kind 8 with different economic fields | `fsd_outstanding_A` −= a; `false_A` += a; the pool burns up to a, once, reducing `false_A`; HARD freeze; penalty evidence |
| PAID | The key's kind 8 after REFUTED | Fields differ: mints the proven tuple normally (caps; payee and queue only with a revived LP). Fields equal: nothing is minted; `reserve_A` += a; re-credits the pool's burn and clears `false_A` |

A mismatching kind 8 on an ATTESTED key applies REFUTED and PAID together. A
kind 8 on a consumed nullifier is valid only for an ATTESTED or REFUTED key,
once (the kind 8 rules, r10 node changes §5.3 checks 5-6, must say so).
The invariant becomes supply ≤ Σ (reserve_v + transit_v) +
`fsd_outstanding_A` + `false_A` (false mints not yet burned).

### 4.5 Refutation and deadlines

**Order facts [E].** Each Vault_A deposit takes the next deposit id and the
next global Outbox position (r8 contracts spec §6.2-§6.3;
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
after ts (kind 11, r10 node changes §8.3) covers p or refutes by 15(c), so
every key resolves about 7.5-16 days after its deposit, plus the grace.

**Timeout.** A key not passed by any proven position after
`fsd_reconcile_blocks` (default 30,240: 21 days at 60 s, ~27 at 78 s) makes
the lane SOFT: confirmations or heartbeats have stopped, which also closes
Vault_A's gate (A2), and the next proven heartbeat resolves the key.

### 4.6 Freeze

| State | Trigger | New FSDs | Ends |
|---|---|---|---|
| OPEN | — | Yes | — |
| SOFT | Timeout; Vault_A impaired or gate closed; finality lag > 90; a pending Vault_A migration (§4.7) | No | By itself once Arbitrum confirms and the cause clears |
| HARD | A REFUTED key; metadata mismatch; quorum equivocation; kind 13 | No | **Only a coordinated hard fork** with a new FSD tag (B-29) |

Only consensus evidence freezes; no key, owner or vote can freeze or
unfreeze. Settlements and refutations always continue.

### 4.7 Cap and first-loss pool

**Pool [I]:** keyless bUSD in consensus state; anyone may contribute. With
no LP float to seed it (r8 :365), FSD opens only once someone, likely the
owner's single-key treasury, funds it. Contributors bear losses pro
rata. **Withdrawals** complete after `fsd_reconcile_blocks` notice, only if
the remaining pool covers `fsd_outstanding_A` + `false_A`; a HARD freeze locks them until the
burn is applied.

**Cap rule:** `fsd_outstanding_A + amount ≤ min(FSD_CAP_A, pool − pending
withdrawals)`. It caps what is unsettled, not flow, so it holds through
~14.7-day disputes; normally it acts per ~6.5-day window (five epochs of
Vault_A caps [computed]). Above it: a revived LP, or the ~6.4-6.5-day
path.

**Migration.** Outstanding FSD amounts are not in `reserve_A`, so a Vault_A
migration leaf (node changes §7.6) would leave their tokens behind: the
migrating hard fork closes the lane (SOFT) and its leaf waits until
`fsd_outstanding_A` is zero.

### 4.8 Exactly-once, and LPs if revived

Each key pays its proven economic tuple exactly once, by an FSD that
settles or by a kind 8; a refuted FSD is a pool-covered loss, never a second
payment. The rules below apply only if the optional LP fast-fill (r8's B-5,
architecture §15) also returns:

- **LP before FSD:** the FSD resolves the payee as kind 8 does (r10 node
  changes §6), repaying the LP in ~16-29 min.
- **LP after FSD:** fills for an ATTESTED key, or with an FSD in the same
  block, are invalid (r10 node changes §6 rules 1, 5, extended).
- **Buckets stay until SETTLED or PAID**, so after a refutation the kind 8
  still pays an LP that filled the real tuple.

### 4.9 Penalties FSD needs (mainnet gate)

- **FSD bond [I]:** signing needs a separate bonded output outside committee
  weight, so a slash never changes membership or stalls finality. It stays
  locked until every FSD the key signed resolves, and for
  `fsd_reconcile_blocks` after it leaves the committee.
- **Automatic, B3-provable:** 15(a), and kind 13 under P3 only (V1 voting can
  honestly export an orphan, finality §0 item 5): 100% of the bond.
- **Arbitrum-dependent** (REFUTED keys): B3 cannot tell a lie from a Security
  Council rewrite or a Nitro bug all signers share, so each attesting key
  forfeits a bounded share [10%] of its bond after a 30-day delay; an equal
  kind 8 cancels it, and a coordinated hard fork may void it. Stake weight is
  never slashed.
- **Proceeds** are B3, not bUSD, so they burn: deterrence, not dollar
  insurance (Codex review; architecture §10).
- **Condition:** a dual quorum's bonds should exceed 3 × `FSD_CAP_A` in
  value; B3 has no price feed, so this is a ruling, not a rule.
- **Reward:** none for signing (finality §10.4); FD-16.

### 4.10 Latency

**[H]** Batch posted 0-3 min, Ethereum finality 13-19, signing 0.5-2,
light-client update then FSD 2-4 blocks (t_chain, architecture §8.1):
**16-29 min**, against the later route (c)'s 15-26 with an LP (r8 :342) and
launch route (b)'s 17-28 [H] (architecture §8.2), so FSD helps only a later
Vault_A. FlowMesh V1 adds 40-42. Assumes full participation and ~78 s
blocks (O8); a B3 reorg undoes an FSD like any mint.

---

## 5. Contract side

Nothing beyond the later Arbitrum vault stage: its Vault_A V2 (architecture
§17 item 1) emits one `sendTxToL1` per deposit (r8 contracts spec §6.2
step 7), carrying all an FSD and its proof need.
Withdrawals and launch contracts are unchanged; no contract reads an FSD.

---

## 6. Risks and bounds

| Risk | Bound or answer |
|---|---|
| Worst-case loss | The effective cap; the first refutation HARD-freezes the lane |
| Who absorbs it | Pool contributors, then every bUSD holder, first come first served (r8 §12 row 14) |
| Detection delay | ~7.5-16 days plus grace while Arbitrum confirms; longer only keeps the lane SOFT |
| Arbitrum governance, Nitro bug | A force-confirmation could settle a false FSD, a rewrite or shared bug refute honest ones; B3 cannot tell [I]. Cap, pool, freeze and penalties outside committee weight keep finality safe; one client is a shared-bug risk |
| Shared RPC; sync committee | Can deceive honest signers or anchors; loss stays within the cap |
| Liveness | A silent third halts the whole bridge (architecture §13 row 1), FSD included; deposits still mint later by kind 8 |
| Disclosure | If B-25 is ruled, architecture §10's "minted only from Ethereum light-client proofs" adds "or, for Arbitrum deposits up to a cap, from B3 stakers' signatures those proofs later check; a false one is a loss up to that cap, borne first by a first-loss pool" |

---

## 7. Stage placement and effort

- **Mainnet: stage S10, FSD** (S10 and RT7b reserved, architecture §17
  item 2), after the later Arbitrum vault stage
  (architecture §17 item 1; post-launch, so after S9) has run kinds 7 and 8
  live, a **penalties stage** (FSD bonds, V2-D4 changed) after S5 (P3), a
  funded pool and B-36's stake gate. Its hard fork pins `FSD_CAP_A`, the
  Bridge and Outbox, and the activation height.
- **Regtest: RT7b**, `feature_b3_bridge_rt7b_fsd.py`, after RT7, which
  returns only with the Arbitrum vault stage (r8 S4), under the staging
  signer's committee, with a mock Bridge on anvil 31337 and a `finalized`
  view of 31338. Tests cover every §4.4 transition, kind 15 case, deadline
  and §4.3 refusal, pool limits, re-signing, reorg, `-reindex` and FlowMesh
  V1 credit. RT8 and RT9 would add P3 and a refutation drill.
- **Effort [H]**, one engineer, ~4.6-6.7k lines with tests: **8-12
  person-weeks** to RT7b, plus the unestimated Arbitrum vault stage
  (architecture §11); bonds and slashing 5-10 more; 2-4 weeks of node audit,
  no cost figure.

---

## 8. Owner decisions

All are **deferred** by the 2026-10-03 simpler-plan ruling (architecture
§15). Defaults in brackets; the handoff forbids inventing economic rules
(`doc/design/b3-master-handoff.md:1160`), so each number needs a ruling.

| ID | Decision [default] |
|---|---|
| **B-25** | **Bounded OD-8 exception:** committee attestations may mint Arbitrum deposits within the effective cap, always settled or refuted by Outbox proofs [**explicit ruling needed if ever pursued**; reverses architecture §0, §15 B-3, §16 item 3 and README "The idea" and "Ruled"] |
| B-26 | Signers = the B-22 committee (itself awaiting explicit approval); no separate 3-of-4 or 2-of-3 set [yes; accepts the contracts' successor-capture risk, §3] |
| B-27 | Effective cap min(`FSD_CAP_A`, free pool) on unsettled amounts [10,000 bUSD pilot; 50,000 after audit] |
| B-28 | Keyless first-loss pool; withdrawals bounded by outstanding amounts; FSD opens once it is funded, likely by the treasury [yes] |
| B-29 | Freeze [HARD per §4.6, lifted only by hard fork with a new tag; SOFT otherwise, self-clearing] |
| B-30 | Penalties before mainnet FSD [bonds as §4.9: 100% for 15(a) and, under P3, kind 13; 10%, voidable, for refutations; stake never slashed; changes V2-D4/FD-13] |
| B-31 | `fsd_reconcile_blocks` [30,240]; `fsd_settle_grace` [2,880]; time window [−48 h/+768 s]; kind 7 freshness [2,880]; epoch grace [90] |
| B-32 | Batch finality and Outbox pins checked in consensus [yes] |
| B-33 | No FSD fee; wallet `maxLpFeePpm` only with a revived LP [r8's 30 bps; 0 only with a warning and shown headroom] |
| B-34 | Signers run their own Arbitrum and Ethereum nodes [policy]; reward under FD-16 |
| B-35 | S10 after the Arbitrum vault stage, penalties and a funded pool; RT7b after a revived RT7 [yes] |
| B-36 | S10 stake gate [O2 shows the top 3 keys, and separately the owner's keys, below a third of committee weight] |

---

## 9. Conflicts and open items

**Reconcile before any FSD code**, beyond architecture §16 and the
Arbitrum vault stage's OD-8 `:309` and contract §21 overrides: OD-8
`:306` and the texts B-25 reverses (§8); architecture §1, §7, §10, §18;
node changes §1, §7.6, §17 item 1 (kind 8; §4.4 above); finality §9.4 and
§10.2 ("no slashing"); master handoff :285 and :956; rule 3
(`CLAUDE.md:87-88`: bonds change Modern PoS), within the one consensus
override.

**Open:** B-37 erases each committee key after its handover, so the 15(a)
bar and FSD bonds (§4.5, §4.9) must attach to the member, not a retired key;
Bridge and Outbox slots and pins [H, O10]; proof sizes [H, O12]; whether Nitro serves `finalized` as documented [H];
B3's price for the bond condition; whether kind 7 freshness pauses FSD too
often during disputes; late kind 8s after a migration rely on node
changes §7.6's sweep; no delegation for small stakers.

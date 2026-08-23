# B3 Modern PoS — V1 specification (FROZEN)

**Status: V1 FROZEN — implementation authorized (owner rulings, 2026-08-20).**
This document supersedes the earlier VRF/slot/epoch "design base" revision of
itself. The owner explicitly replaced that direction (ruling M1) with the
deterministic stake-weighted design below and authorized a complete V1
implementation. Everything the earlier revision reserved for simulation-locked
numerics is carried here as **provisional constants** marked
`REVISABLE_BEFORE_MAINNET`, kept in one configurable place
(`src/consensus/modern_pos_params.h`) and never configured on mainnet until
explicitly ratified. Advanced mechanisms are **V2 research** (§10) and are
deliberately absent from V1: **no VRF, no epochs, no committees, no slashing,
no finality gadget, no delegation.**

Owner rulings incorporated (2026-08-20):

- **M1** — deterministic stake-weighted hash eligibility plus recovery rounds
  replaces the VRF/slot/epoch direction; this document is the reconciliation.
- **M2** — the eligibility seed is a chained digest (§3); the bounded
  residual grinding surface is recorded in §7; future randomness upgrades
  remain possible without changing the output format.
- **M3** — every modern-PoS block carries a BIP340 validator signature over a
  domain-separated digest of the block hash (§5).
- **M4** — no unstake cooldown in V1: spending the STAKE output removes the
  stake; recreated stake re-serves the 20-block activation depth.
- **M5** — fork choice is PoS-native and explicit (§6); no PoW chainwork
  semantics and no automatically inherited legacy reorg rule.
- **M7 (owner ruling 2026-08-23 — amends M1's "no finality gadget")** — the
  V1 architecture includes **from M** a BLS12-381 finality gadget and BLS
  validator keys, realized as Modern policy cells plus Modern Payload Area
  records: `FINALITY_CERT = 6` (coinbase metadata cell, ≤ 1 per block,
  commitment = hash of the certificate payload carried as MPA record type 4)
  and `FINALITY_KEY = 7` (validator-binding metadata cell: `validator_key`
  commitment, `bls_pubkey ‖ seq` params, identity-authorized by a BIP340
  signature plus a separate BLS proof-of-possession in MPA record type 5,
  sequence-controlled rotation/revocation, effective at the next snapshot),
  with `MODERN_PAYLOAD_ROOT = 8` committing all MPA bytes into the block hash.
  Epochs from M, one-epoch-lookahead snapshot, handover-gated rotation, the
  frozen `ValidatorSetHeader` / `FinalizedBlock` / `Certificate` layouts, and
  the finality pin. **F = M**: these rules ship in the X-pin Modern-PoS
  release (the first binary that validates any modern-era block); bridge use
  is a later flag A3 ≥ F. STAKE v1, M1–M6 and the block wire format are
  unchanged. Specification: [b3-cross-chain-finality-v1.md](b3-cross-chain-finality-v1.md)
  (normative), [b3-modern-payload-area.md](b3-modern-payload-area.md)
  (carrier), [b3-finality-to-ethereum.md](b3-finality-to-ethereum.md)
  (rationale). §10's "committee/finality gadgets" entry is superseded.
- **M6** — a block reward can never directly create active STAKE: the
  coinbase may not contain a STAKE-claiming output.
- Plus: the unconditional modern coinbase cap (§8) sits **outside** the PoS
  validator design and applies regardless of any future rule set.

## 1. Inherited constraints — unchanged

- Modern block header stays Bitcoin-style (80 bytes, SHA256d identity); no new
  hash domain for block identity (contract §17).
- Modern fork choice selects only among descendants of X; reorganizations
  across H are prohibited; the corridor design
  ([b3-during-fork-transition.md](b3-during-fork-transition.md)) is unchanged:
  modern PoS begins at M = H + corridor length + 1.
- Value lives in typed Policy Outputs; STAKE is `PolicyType::STAKE = 4` with
  the v1 script carrier in `src/modern/stake.h` — **RATIFIED 2026-08-21
  exactly as implemented and tested** (B3S1 payload, validator key,
  owner-controlled funds).
- `ModernChainDomain = TaggedHash("B3/MODERN/CHAIN", genesis || X)` is the
  anti-replay domain for new signed structures — never block identity.
- Validators and FlowMesh FNs are separate roles. Modern issuance carries an
  explicit cap (§8).

Carried forward unchanged from the superseded revision (they are compatible
with M1): STAKE is a Policy Output; independent per-wallet STAKE outputs;
locked amount is the weight; owner key ≠ validator key; rewards never
auto-increase weight (now consensus-enforced, M6); no coin-age growth;
aggregation is per validator key, never per output (splitting confers
nothing); eligibility is cheaply verifiable from the block plus the parent's
derived registry.

## 2. Stake data model

Exactly the machinery already in the tree:

- **Carrier**: `PUSH(38: "B3S1" || validator_key[32] || reserved[2]=0)
  OP_DROP <owner script>` (`src/modern/stake.h`), enforced per-transaction in
  consensus (`modern::CheckStakeOutputs`), fail-closed while
  `min_stake_amount` is unset.
- **Maturity**: a STAKE output created at height `b` is ACTIVE at height `h`
  iff `h − b >= STAKE_ACTIVATION_DEPTH (= 20)`.
- **Registry**: the set of unspent, modern-era, valid STAKE outputs. Per
  validator key: `w` = sum of ACTIVE principal. `W` = total ACTIVE principal.
  Derived state, recomputable from the chain; the node maintains it
  incrementally at tip changes and rebuilds it by walking modern-era blocks
  after a restart or reorg (`src/node/stake_tracker.{h,cpp}`).
- **Unstake (M4)**: spending the output removes it; nothing else exists.

## 3. Seed chain and eligibility

```
seed_M        = TaggedHash("B3/MODERN/POS/SEED/V0",
                           ModernChainDomain || parent_marker_hash)
                (parent = the last pre-modern-PoS block: the corridor-exit
                 block, or H itself when the corridor length is 0)

digest_h      = TaggedHash("B3/MODERN/POS/ELIG/V1",
                           ModernChainDomain || seed_h || height || round
                           || validator_key)

seed_(h+1)    = digest_h            (the accepted block's own digest)
```

Eligibility of validator `v` with aggregated ACTIVE weight `w` at height `h`,
round `r`:

```
digest_h  <  MAX256 · f(r) · w / W        with   f(r) = f0 · 2^r
```

evaluated in widened integer arithmetic (`digest · W · f0_den` vs
`2^256 · w · f0_num · 2^r` over 512-bit integers, saturating toward
"eligible" when the right side overflows — a saturated round admits every
online validator, which is exactly the recovery intent). `w = 0` is never
eligible. There is **no difficulty retarget**: the `w / W` normalization is
the difficulty, adjusted exactly and instantly by the registry itself.
`nBits` is a fixed enforced sentinel and `nNonce` must be 0.

One attempt exists per validator key per (height, round). Block content —
transactions, ordering, coinbase bytes, merkle root, signature — has **zero**
influence on the seed.

## 4. Deterministic timestamps and recovery rounds

```
nTime = parent.nTime + BLOCK_INTERVAL + round · ROUND_SECONDS     — EXACTLY
```

The round is decoded from the timestamp delta; a delta that is not
`BLOCK_INTERVAL` plus a non-negative exact multiple of `ROUND_SECONDS` is
invalid (`bad-pos-time`). There is no stored round field and no upper round
bound: arithmetic saturation (§3) makes very high rounds admit everyone, so a
stall of any length resolves.

Consequences:

- The existing future-time rule (tightened to `max_future_seconds`) is the
  pacing gate: a round's block cannot be accepted before its forced timestamp,
  so the chain can never run faster than one block per `BLOCK_INTERVAL`, and
  rounds advance in real time during a genuine stall — chain time tracks wall
  time within one round.
- A lagging node and a stalled network are indistinguishable and behave
  identically: keep evaluating successive rounds against the best tip.
- MTP monotonicity is trivially satisfied (each block adds at least
  `BLOCK_INTERVAL`); the stock MTP check is retained as belt-and-braces.

## 5. Block structure and signature (M3)

The 80-byte header is untouched. Two body rules:

- **Validator declaration**: the coinbase scriptSig is
  `BIP34 height push || PUSH32(validator_key)` — the key is merkle-committed
  through the coinbase. Malformed or missing declaration: `bad-pos-key`.
- **Block signature**: marker-modern blocks serialize a trailing
  `vchBlockSig` vector (the legacy codec's own trailing-signature pattern).
  Corridor blocks must carry it **empty**; a modern-PoS block must carry
  exactly 64 bytes: a BIP340 signature by `validator_key` over
  `TaggedHash("B3/MODERN/POS/SIG/V1", ModernChainDomain || block_hash)`.
  Since the block hash commits to the header, the merkle root, and therefore
  the coinbase-declared key, the binding is complete and the
  sign-your-own-hash circularity is resolved exactly as the legacy codec
  resolved it. Invalid: `bad-pos-signature`.

This revises the modern block wire format before any modern block exists
outside regtest fixtures; it cannot be revised after mainnet H/X.

## 6. Fork choice (M5) and reorganization horizon

PoS-native rule over descendants of X:

1. **Higher valid height wins.** (Implementation note: modern-PoS blocks
   accumulate a constant per-block increment in the existing `nChainWork`
   accumulator — the sentinel `nBits` makes `GetBlockProof` a constant — so
   the existing most-work selection already implements height-first among
   modern chains. The number is bookkeeping, not work.)
2. **Equal height** → at the first divergent block, the **lower recovery
   round** wins (provably rarer eligibility; the claimed round sets the
   threshold the digest actually beat, and the round is timestamp-derived,
   not declared).
3. Equal round → **lower block hash** wins (deterministic, unpredictable).

**V1 deviation, recorded:** the owner-analyzed rule 3 was "lower normalized
eligibility score (`digest/weight`, cross-multiplied)". Candidate ordering in
the node must use only data immutable from the moment a block enters the
candidate set, and both the digest's seed context and the validator weight
are connect-time state; mutating comparator keys while an element is in
`setBlockIndexCandidates` is undefined behavior. V1 therefore resolves the
double tie by block hash — equally deterministic, unpredictable in advance,
security-equivalent; the normalized-score refinement is listed in §10 for V2.
Rule 2 uses the timestamp delta (immutable at acceptance) and is implemented
exactly.

**Horizon**: a modern-PoS reorganization deeper than `modern_reorg_horizon`
(D) blocks is refused without peer penalty (`modern-reorg-too-deep`,
modeled on — not inherited from — the legacy depth bar; skipped during
reindex/import). D is an owner parameter, deliberately unchosen; V1 regtest
uses a scaffolding value. Combined with the timestamp density property —
a private rebuilder holding stake fraction `a` pays
`BLOCK_INTERVAL + E[rounds|a] · ROUND_SECONDS` of forced chain time per
block over the same wall-clock span, so a small-stake attacker cannot reach
the honest height — this is the complete V1 long-range defense for online
nodes. **Known accepted residual:** a fresh-sync node's post-M history has
the classic weak-subjectivity exposure to an attacker holding a majority of
*historical* stake; X pins everything pre-M; operational checkpoints can
cover the rest without protocol machinery. No slashing/BFT ships in V1.

## 7. Grinding and attack surface (M2, recorded)

- Content grinding (tx selection/ordering, coinbase bytes, merkle, nTime):
  **eliminated** — the seed excludes block content and nTime is forced.
- Single-key producer: their digest, and thus the next seed, is fully
  determined — zero grinding.
- Multi-key producer: one seed option per simultaneously-eligible own key
  (~log2(k) bits), stake-bounded.
- Round-delay grinding: forfeits fork-choice priority (rule 2) to any
  competitor; costly and visible.
- JIT stake targeting: dead — the seed is knowable one block ahead; stake
  needs 20 blocks to activate.
- Corridor-exit seed (`seed_M`): grindable only at the cost of real scrypt
  solutions at corridor difficulty; influences one seed. Accepted for V1.
- Far-future prediction: certainty extends exactly one block ahead (each
  winner's key re-randomizes the seed); no far-future proposer DoS window.
- Withholding: ~one-block window; height-first makes it unprofitable.
- Equivocation: resolved by the deterministic tiebreak; BIP340 signatures
  leave attributable evidence for a V2 penalty rule; V1 imposes none.

## 8. Rewards and the unconditional cap

- **Cap (unconditional, outside the validator):**
  `coinbase value out ≤ fees + modern_reward(height)`; with the V1 parameter
  block unset or its reward at 0, the cap is fees-only. Enforced in
  `ConnectBlock` for every modern-PoS block **before and independently of**
  any PoS rule set, so no future validator can bypass it (`bad-cb-amount`).
- **M6:** any coinbase output claiming the STAKE magic is invalid
  (`bad-cb-stake`); rewards pay ordinary outputs; restaking is an explicit
  STAKE output plus the activation depth.
- The reward schedule and all amounts remain owner parameters
  (`REVISABLE_BEFORE_MAINNET`; V1 provisional: 0 — fees only).

## 9. Parameters — one place, all provisional

All in `src/consensus/modern_pos_params.h`, marked `REVISABLE_BEFORE_MAINNET`,
configured **only** by test fixtures; mainnet params never set the block, and a
guard test enforces that. Unset ⇒ modern-PoS validation and production fail
closed (`no-modern-pos-rules`), exactly as before this specification.

| Parameter | Value | Status | Meaning |
|---|---|---|---|
| `block_interval_seconds` | 60 | **RATIFIED 2026-08-21** | forced spacing floor |
| `round_seconds` | 30 | **RATIFIED 2026-08-21** | recovery-round length |
| `f0_num / f0_den` | 1 / 1 | **RATIFIED 2026-08-21** | round-0 expected eligible ≈ f0 · online fraction |
| relaxation | ×2 per round | **RATIFIED 2026-08-21** (fixed in V1) | eligibility doubling |
| `sentinel_bits` | 0x207fffff | provisional | enforced constant `nBits` |
| `max_future_seconds` | 120 | provisional | clock-skew allowance / pacing gate |
| `reward` | 0 (fees only) | provisional (OD-2) | per-block subsidy under the cap |
| `reorg_horizon` (D) | 1440 | **RATIFIED 2026-08-21** (one day at 60 s) | modern reorg refusal depth |

The ratified rows are the confirmed V1 numbers (min stake is likewise
ratified: 333 modern B3 = 333e9 base units, stated on mainnet, inert until
H/X); the parameter block still ships unset on every network until the
remaining provisional rows are settled, so nothing activates piecemeal. The STAKE v1 carrier is likewise RATIFIED
(2026-08-21) exactly as implemented and tested.

Timing behavior at f0 = 1 (from the accepted analysis): ~63% of blocks in
round 0 at full participation; 95% by round 2/3/4/5/8 at 100/50/25/10/1%
online stake; even 1% online stake produces a block every few minutes with no
intervention.

## 10. V2 research (explicitly out of V1)

Normalized-score tie resolution (§6); VRF or threshold randomness upgrades to
the seed; epoch snapshots and incremental-registry commitments for light
clients; equivocation penalties consuming the V1 evidence trail; unstake
cooldown; validator-key re-delegation; ~~committee/finality gadgets~~ (now
V1-reserved by M7); reward
schedule economics; fresh-sync weak-subjectivity hardening beyond operational
checkpoints. None of these may be implemented without a new owner ruling.

## 11. Implementation map (V1)

`consensus/modern_pos_params.h` (parameter block) · `modern/pos.h`
(deterministic validator replacing the fail-closed stub; the test-only
injection hook remains for dispatch-plumbing tests and is never set in
production) · `primitives/block.h` (modern trailing-signature codec) ·
`chain.h` (per-index cached eligibility digest, persisted; a restart must not
recompute seeds from block bodies) · `node/stake_tracker.{h,cpp}` (registry
maintenance) · `validation.cpp` (header rules: sentinel bits, exact time,
horizon; connect rules: cap, M6, eligibility, signature; comparator rule 2/3)
· `node/miner.cpp` (deterministic production + signing). Regtest scenarios:
normal operation, low-online-stake recovery, invalid signature, invalid
eligibility, invalid reward, restart/reindex.

# B3 Modern PoS finality → Ethereum (specification, revision 2)

**Status: committed design record, revision 2 (2026-08-23), written to the owner's
rulings of the same day: (i) BLS finality is NOT a V2-only feature — the Modern PoS V1
architecture reserves the finality object and BLS validator keys **from the modern era's
genesis (M)**, bridge activation being a later feature flag; (ii) no 3,500 individual
signatures, no MPC/TSS with hidden signers; (iii) the Ethereum verifier follows
`genesis validator set → epoch certificates → validator rotation → withdrawal roots`.
Revision 1 (a "V2 layer") is superseded. "Do not implement yet" — this is specification
only; the reconciliation amendment to the frozen PoS V1 spec is recorded as ruling **M7**
in [b3-modern-pos-spec.md](b3-modern-pos-spec.md).**

**Superseded in detail by the 2026-08-23 rulings (cells 6/7/8 + MPA, identity-authorized binding, gated rotation, F = M): see the normative spec rev. 2. Normative protocol text: [b3-cross-chain-finality-v1.md](b3-cross-chain-finality-v1.md)** — exact layouts, the fixed-depth validator-set Merkle commitment, and the Ethereum verification algorithms for epoch transition, weights, bitmap, quorum and withdrawal root. This document is the rationale/attack record; where the two differ, the normative document governs (one known refinement: ONE cumulative withdrawal tree whose leaf carries `origin_chain_id`, not one tree per origin chain).

Deposits (Ethereum → B3 via receipt/state proofs) are unchanged from
[b3-bridge-bls-proposal.md](b3-bridge-bls-proposal.md) §2.2 and not discussed here.

---

## 0. Three heights, one lineage

| Symbol | Meaning | Status |
|---|---|---|
| **M** | first modern-PoS block (821,001 under the 2026-08-23 H ruling). **Epoch 0 begins here.** Binding actions are valid from H+1 (the corridor), certificates from M. | ruled |
| **F** | finality-enforcement height: from F, certificate cryptography and the finality pin are consensus rules. **F = M is the target**; if the v1 binary cannot carry `blst`, F is pinned by the mandatory X-pin follow-up release (same pattern as X). | owner (§9) |
| **A3** | bridge activation (`BRIDGE_BACKED`, `BRIDGE_BURN`, withdrawal tree, Ethereum contracts live). **A3 ≥ F.** | owner, later |

What is **reserved from M regardless of F**: the two creation-action types, the epoch
arithmetic, the set snapshot rule, the set-header/leaf encodings, the `FinalizedBlock`
layout with `withdrawal_root = 0x00…00` until A3, the signing digest, the coinbase
certificate slot. The lineage of validator sets therefore starts at **Set_0 at M** and
Ethereum's trust root is the **modern-genesis validator set**, not a mid-chain checkpoint.

---

## 1. Objects (frozen V1 layouts — keccak where Ethereum recomputes)

```
ValidatorSetHeader {                       // 126 bytes, fixed, big-endian
  epoch              u64
  ruleset_version    u16      // quorum rule id (1 = §4); new numbers never change layouts
  validator_count    u32      // n, 1 ≤ n ≤ MAX_FINALITY_SET (8,192 proposed)
  total_weight       u64      // W = Σ w_i, in whole modern B3 (w_i = floor(active_base_units / 1e9))
  quorum_weight      u64      // floor(2·W/3) + 1        (ruleset 1)
  aggregate_pubkey   48 B     // Σ_i pk_i in G1 (unweighted), consensus-computed
  members_root       32 B     // keccak Merkle root of leaf_i, zero-padded to 2^⌈log2 n⌉
}
leaf_i             = keccak( u32 index_i ‖ pk_i(48) ‖ u64 w_i )      // sorted by validator_key
validator_set_hash = keccak( ValidatorSetHeader )

FinalizedBlock {                           // 120 bytes, fixed
  height             u64
  block_hash         32 B     // modern block hash
  withdrawal_root    32 B     // §7 accumulator root; all-zero before A3
  validator_set_hash 32 B     // keccak(header of the SUCCESSOR set, Set_{epoch+1})
  epoch              u64      // epoch of the SIGNING set, Set_epoch
}
finality_digest    = TaggedHash("B3/FINALITY/V1", ModernChainDomain ‖ FinalizedBlock)

Certificate {
  signer_bitmap      bitvector[n]  (⌈n/8⌉ bytes, index = set index, unused bits zero)
  aggregate_sig      96 B          (G2)
}
```

BLS ciphersuite: `BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_` (pubkeys G1 48 B,
signatures G2 96 B, PoP DST `BLS_POP_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_`) — the
Ethereum variant, natively checkable with EIP-2537.

---

## 2. Validator key lifecycle

A validator has **two keys, one identity**:

| Key | Where | Role |
|---|---|---|
| `validator_key` (32 B, BIP340 x-only) | STAKE carrier `B3S1` (ratified v1, unchanged) and the coinbase declaration (M3) | stake identity, block signature |
| `bls_pubkey` (48 B) | `VALIDATOR_BLS_BINDING` action | finality signature |

**Binding** — creation action **type 5 / version 1** (`CREATION_ACTION_VALIDATOR_BLS_BINDING`),
payload exactly `{validator_key 32 ‖ bls_pubkey 48 ‖ pop 96 ‖ bip340_sig 64}` = 240 B,
valid in any modern-era block (corridor included, so Set_0 can be non-empty at M):

- `pop` = BLS signature by `bls_pubkey`'s secret over `bls_pubkey` under the PoP DST
  (rogue-key defence);
- `bip340_sig` = signature by `validator_key` over
  `TaggedHash("B3/FINALITY/BIND/V1", ModernChainDomain ‖ validator_key ‖ bls_pubkey)`
  (proves the staker controls the finality key — no third party can bind a key to
  someone else's stake);
- binding state = `validator_key → (bls_pubkey, bound_height)`; **latest wins**; undo on
  disconnect; rebuilt on reindex like the stake registry;
- a `bls_pubkey` may be bound by **one** validator key at a time (duplicate ⇒ invalid
  action) so the aggregate pubkey never double-counts a key.

**Lifecycle states** (per validator key):

```
UNBOUND ──bind──► BOUND ──(stake ACTIVE ≥ 20 blocks, w > 0)──► ELIGIBLE-AT-SNAPSHOT
   ▲                │                                                  │
   │                └──rebind (new pk, effective at next snapshot)──────┘
   └──────────────── unstake (M4: spend) ⇒ w = 0 ⇒ drops out at next snapshot
```

- Block production (M1/M3) does **not** require a binding in V1; the finality set is the
  bound, ACTIVE, `w > 0` validators. Owner option (§9): require binding for block
  eligibility from F, making `W_fin = W` identically.
- A key rotation is a rebind; an exit is an unstake; both take effect at the next
  snapshot, never mid-epoch (the set in force is immutable for its epoch).
- Equivocation evidence (two signatures by one index at one height, or a signature on a
  non-descendant of a certified checkpoint) is stored by nodes from M; **slashing is not
  in V1** — the evidence trail is the V2 input (spec §10).

---

## 3. The finality gadget — exact rules

### 3.1 Epochs

- `E = FINALITY_EPOCH_BLOCKS` (1,440 proposed = the ratified reorg horizon). Epoch
  `e ≥ 0` = heights `[M + e·E, M + (e+1)·E)`. Epoch of height h: `(h − M) div E`.
- **Snapshot rule (lookahead 1):** `Set_{e+1} = Snapshot(last block of epoch e−1)` for
  `e ≥ 1`; **`Set_0 = Set_1 = Snapshot(M − 1)`** (the corridor-exit block). `Snapshot(b)` =
  all validator keys with a binding and ACTIVE stake at height b, sorted ascending by
  `validator_key`, with `w_i = floor(active_base_units_i / 1e9)`, dropping `w_i = 0`.
- **Carry-over rule:** if a snapshot would have `n < MIN_FINALITY_SET` (4 proposed) or
  `W < MIN_FINALITY_WEIGHT` (owner), then `Set_{e+1} = Set_e` with the header's `epoch`
  re-stamped — the lineage never has a hole. A carry-over may repeat at most
  `MAX_CARRY_OVER` (7 proposed) consecutive epochs; beyond that the lineage is declared
  **broken** on B3 (no further certificates are valid) and only a consensus re-bootstrap
  (§3.5) restarts it. (Bounds the "stale set with nothing at stake" exposure, §8-A3.)
- The set header for every epoch is consensus-derived state (`finality_set[e]`), exposed
  by RPC, and recomputable from the chain by any node.

### 3.2 Checkpoints and signing (validator behaviour, node-enforced from M)

- Checkpoint heights: `h ≡ 0 (mod CHECKPOINT_INTERVAL)` relative to M (60 proposed) — and
  the last block of every epoch is always a checkpoint (so every epoch has ≥ 1).
- A validator signs checkpoint `h` once `tip_height − h ≥ CHECKPOINT_DEPTH` (20 proposed).
- **Signing rules (safety):** a validator (i) signs **at most one checkpoint per height**,
  (ii) signs only heights **greater than its last signed height**, and (iii) signs only
  checkpoints that are **descendants of the latest certified checkpoint it knows**. The
  signing set for height h is `Set_{epoch(h)}`; `FinalizedBlock.epoch = epoch(h)`,
  `validator_set_hash = hash(Set_{epoch(h)+1})`, `withdrawal_root` = accumulator root at h
  (zero before A3).
- Message: `finsig {epoch u64, height u64, index u32, sig 96}` on P2P; relay only for
  bound indices of the named set, one per (index, height), bounded queue; anyone
  aggregates (G2 addition; bitmap OR). No leader, no round, no timeout — a checkpoint
  is certified the moment a quorum of signatures exists anywhere.

### 3.3 Quorum calculation (ruleset 1)

```
W              = Σ_{i<n} w_i                    (u64, whole modern B3)
quorum_weight  = floor(2·W / 3) + 1
signed_weight  = Σ_{i ∈ bitmap} w_i
certified      ⇔ signed_weight ≥ quorum_weight ∧ BLS.FastAggregateVerify(pks[bitmap], digest, sig)
```

Two certificates for conflicting checkpoints at the same height each carry ≥ 2W/3 + 1,
so their signer sets overlap in ≥ W/3 + 2 weight; if Byzantine weight is < W/3 at least
one honest validator is in the overlap, and honest validators never sign twice at one
height (§3.2 i). **Safety needs Byzantine weight < W/3; liveness needs honest-online
weight ≥ 2W/3 + 1.** Head-count is irrelevant; one validator with 70 % of the stake is
the (correct, economic) quorum. Quorum changes = a new `ruleset_version` whose number
appears in signed headers; Ethereum verifies the stored `quorum_weight`, never the rule.

### 3.4 The certificate on B3 (consensus from F; syntactic from M)

- Creation action **type 4 / version 1** (`CREATION_ACTION_FINALITY_CERTIFICATE`),
  payload `FinalizedBlock ‖ Certificate` (120 + ⌈n/8⌉ + 96 B ≤ 4,000 B ⇒ n ≤ 30,272;
  `MAX_FINALITY_SET` keeps it far below). **Placement:** only in the **coinbase**
  transaction's creation-action section, **at most one per block**. This uses the
  existing bounded section — no block wire-format change, which M3 forbids after H/X.
- **From M (syntactic, v1 binary):** placement, length, bitmap width = `n` of the named
  epoch's set, `epoch ≤ current epoch`, checkpoint height is a checkpoint height and an
  ancestor of the block. Anything else ⇒ `bad-finality-cert-form`, block invalid.
- **From F (cryptographic + pin):** quorum (§3.3) and `FastAggregateVerify` ⇒ else
  `bad-finality-cert`, block invalid. On connect of a block carrying a valid certificate
  for checkpoint C: `finalized_tip = max(finalized_tip, C)`, and **any reorganization that
  would disconnect `finalized_tip` is refused** (`modern-finality-violation`, no peer
  penalty, skipped during reindex/import exactly like the horizon). The pin is what makes
  B3 finality ≥ Ethereum finality — without it a certified burn could be reverted on B3
  after release on Ethereum.
- Producers include the best pending certificate they hold (node behaviour; nothing
  forces inclusion — liveness of *inclusion* only matters for the pin, not for Ethereum,
  which accepts certificates from anywhere). Every node archives the highest certificate
  per epoch it has seen (`finality index`), whether or not it was included.
- Between M and F, certificates are produced, relayed, archived and syntactically
  validated, so the **certificate lineage from epoch 0 exists** and is later verifiable by
  anyone (and by Ethereum) even though B3 did not yet enforce it.

### 3.5 Re-bootstrap (lineage broken)

If `MAX_CARRY_OVER` is exceeded, or the owner pins a new genesis set by a consensus
release, B3 defines `finality_genesis_epoch' = e*` with `Set_{e*} = Snapshot(...)` from
chainparams; Ethereum needs the matching governance re-bootstrap (§6). Expected never to
happen on a healthy chain; specified so it is a rule, not an improvisation.

---

## 4. Epoch transition rules — one table

| At height | B3 does | Ethereum learns |
|---|---|---|
| `M − 1` (corridor exit) | computes `Set_0 = Set_1 = Snapshot(M−1)` | `Set_0` header = the verifier's immutable genesis (constructor) |
| during epoch `e` | validators of `Set_e` sign checkpoints; every `FinalizedBlock` carries `hash(Set_{e+1})` | from the first accepted certificate of `e`: `nextSet = Set_{e+1}` |
| last block of `e` | is a checkpoint (always); `Snapshot` here defines `Set_{e+2}` | — |
| first certificate of `e+1` | signed by `Set_{e+1}` | `currentSet ← nextSet` (rotation), `currentEpoch ← e+1` |
| carry-over | `Set_{e+1} = Set_e` re-stamped | a successor header with the same members and new `epoch` — nothing special |

Strict on both sides: epochs advance `e → e+1` only (each set attests only its
successor). Within an epoch, checkpoints may be skipped freely. A relayer must land
**≥ 1 certificate per epoch** (once a day at E = 1,440) to keep the verifier in lineage;
missed epochs are replayed from any node's finality index.

---

## 5. Ethereum verifier — `B3FinalityVerifier.sol`

```
genesis validator set  →  epoch certificates  →  validator rotation  →  withdrawal roots
```

```solidity
struct SetHeader     { uint64 epoch; uint16 rulesetVersion; uint32 validatorCount;
                       uint64 totalWeight; uint64 quorumWeight;
                       bytes aggregatePubkey /*48*/; bytes32 membersRoot; }
struct FinalizedBlock{ uint64 height; bytes32 blockHash; bytes32 withdrawalRoot;
                       bytes32 validatorSetHash; uint64 epoch; }
interface IB3FinalityProver {   // BLS today, ZK later; same inputs forever
  function verify(bytes32 chainDomain, FinalizedBlock calldata fb,
                  bytes32 setHash, SetHeader calldata set, bytes calldata proof)
           external view returns (bool);
}
```

`submitCertificate(FinalizedBlock fb, SetHeader successor, bytes proof)`:

```
require fb.epoch == currentEpoch
     || (fb.epoch == currentEpoch + 1 && nextSetHash != 0)
if fb.epoch == currentEpoch + 1:  rotate: currentSet = nextSet; currentSetHash = nextSetHash;
                                  currentEpoch += 1; nextSet = ∅; setHashByEpoch[currentEpoch] = currentSetHash
require fb.height > latest.height
require keccak(successor) == fb.validatorSetHash && successor.epoch == currentEpoch + 1
require successor.quorumWeight > successor.totalWeight / 2 && successor.validatorCount ≥ 1   // sanity, not the rule
require prover.verify(chainDomain, fb, currentSetHash, currentSet, proof)
if nextSetHash == 0 { nextSet = successor; nextSetHash = fb.validatorSetHash }
else require nextSetHash == fb.validatorSetHash
require block.timestamp − lastRotationTime ≤ MAX_EPOCH_LAG   // §8-A3 weak-subjectivity bound
latest = fb; emit Finalized(fb)
```

`BlsCertificateProver.verify` (EIP-2537): `proof = abi.encode(bitmap, sig, NonSigner[]
{index, pubkey, weight}, bytes32[] multiproof)`; checks bitmap width/zero-bits/popcount
consistency, multiproof of non-signers against `membersRoot`, `signedWeight = totalWeight −
Σ absent ≥ quorumWeight`, `aggPk = aggregatePubkey − Σ absent` (G1ADD of negations),
`Hm = hash_to_G2(finality_digest)` (SHA-256 `expand_message_xmd` + 2×`MAP_FP2_TO_G2` +
`G2ADD`), pairing check `e(aggPk, Hm)·e(−G1, sig) = 1`. Cost scales with absentees:
≈ 0.3 M gas at 99 % participation, ≈ 0.8 M at 95 %, ≈ 2 M at 80 % for 3,500 validators.

**`ZkFinalityProver.verify`** later: `proof` = SNARK with public inputs
`(chainDomain, keccak(fb), setHash)` proving the same certificate predicate. Nothing on
B3 changes; no struct changes; only the prover address (§6).

---

## 6. What Ethereum stores permanently

| Slot | Mutability | Why permanent |
|---|---|---|
| `chainDomain` (B3 `ModernChainDomain`) | immutable | binds every digest to one B3 chain; anti-replay |
| `genesisSetHeader`, `genesisSetHash`, `genesisEpoch` | immutable | the trust root; lets anyone re-verify the whole lineage from Set_0 |
| `setHashByEpoch[e]` | append-only | audit trail + ZK provers reference any historical set; 32 B per epoch (~20 k gas/day) |
| `currentEpoch`, `currentSet` (header), `currentSetHash` | rotates | the set whose certificates are accepted now |
| `nextSet`, `nextSetHash` | per-epoch | the attested successor |
| `latest` `{height, blockHash, withdrawalRoot}` | monotone | the finalized tip and the only withdrawal root the vault consults |
| `lastRotationTime` | per-rotation | weak-subjectivity lag bound |
| `prover` | governance | BLS → ZK swap point |
| vault: `released[withdrawalId]`, `tokenOf[assetId]` | append-only / governance | double-release guard; asset table |

Not stored: certificates, bitmaps, non-signer data (events only); full member lists
(committed by `membersRoot`; 3,500 × 48 B on-chain would cost ~100 M gas).

---

## 7. Withdrawal roots and release (from A3)

- `BRIDGE_BURN` appends `leaf = keccak(u64 withdrawal_id ‖ bytes32 asset_id ‖ address
  origin_token ‖ address recipient ‖ u256 amount ‖ u64 b3_height)` to a **cumulative
  depth-32 keccak incremental tree per origin chain** (deposit-contract construction;
  append-only; undo on disconnect). Root at height h = `withdrawal_root` of every
  `FinalizedBlock` at h. Before A3 the root is all-zero and the vault does not exist.
- `B3Bridge.release(Withdrawal w, bytes32[32] path)`: `!released[w.id]`; leaf(w) at index
  `w.id` under `verifier.latest().withdrawalRoot`; `tokenOf[w.asset_id] == w.origin_token`;
  mark; transfer. Cumulative root ⇒ old burns prove against newer roots; Ethereum keeps
  only the latest. Optional owner-ruled rate limit / delay sits here.
- **Release ⇔ (finality certificate accepted) ∧ (inclusion proof).** No admin, relayer, or
  signer can release otherwise.

---

## 8. Worst-case attack scenarios

| # | Scenario | Effect | Defence / residual |
|---|---|---|---|
| A1 | Byzantine weight ≥ W/3 refuses to sign | no certificates; withdrawals stall, chain continues | liveness only; no loss. Evidence of abstention is public (bitmap) |
| A2 | Byzantine weight > 2W/3 (collusion or key theft) | forge a `FinalizedBlock` with a fake `withdrawal_root` → drain vault; or hand over to an attacker-only successor set | **the** trust assumption, same as every PoS light client. Bounds: cost = buying/compromising > 2/3 of ACTIVE stake (min stake 333 B3); vault per-epoch caps and delay window (owner); signers are identifiable (slashing V2); governance pause (§6) |
| A3 | Stale set / long-range: verifier not updated for many epochs; an old set's members unstaked and sold keys; attacker signs a fake lineage from the last set Ethereum knows | Ethereum cannot distinguish | `MAX_EPOCH_LAG` on Ethereum (30 days proposed: after that only governance re-bootstrap); `MAX_CARRY_OVER` on B3 (stale sets never persist); relayer keep-alive is cheap (1 cert/day) |
| A4 | Set-jump: a 2/3 set attests a successor of one key | permanent takeover | = A2. Optional overlap rule (successor must share ≥ W/3 weight with current) — owner option; rejected by default because it blocks legitimate mass rotation |
| A5 | Equivocation by < W/3 | nothing certifies twice (§3.3 overlap) | evidence stored; V2 slashing |
| A6 | Honest validator on a 20+-deep minority fork signs a checkpoint there, chain reorgs | it never signs the same height again (§3.2 i); its signature on the dead fork is useless | no conflicting certificate possible without Byzantine ≥ W/3 |
| A7 | Reorg past a certified checkpoint on B3 | refused from F (pin); before F: possible, but A3 ≥ F so no funds at risk | horizon (1,440) bounds it anyway |
| A8 | Rogue-key aggregation | attacker binds `pk_attacker = pk* − Σ honest` to forge aggregate | PoP at binding; Ethereum trusts the consensus-computed `aggregate_pubkey` inside a signed header |
| A9 | Cross-chain / cross-set replay of a certificate | — | `chainDomain` in every digest; `epoch` in `FinalizedBlock`; bitmap width bound to the epoch's `n`; Ethereum requires epoch ∈ {current, current+1} |
| A10 | Relayer censorship / nobody relays | verifier lags; eventually `MAX_EPOCH_LAG` | permissionless relaying; any user with a pending withdrawal is motivated; B3 nodes archive certificates forever |
| A11 | `finsig` P2P spam | bandwidth | only bound indices of the current/next set relay; one per (index, height); bounded queue |
| A12 | Hash-to-curve / encoding mismatch between `blst` and the contract | false negatives (liveness) or, worst, a malleable digest | identical DST, fixed-width big-endian encodings, shared test vectors on both sides; digest is a 32-B tagged hash |
| A13 | Empty or tiny genesis set at M | lineage never starts | owner's own validators bind in the corridor; `MIN_FINALITY_SET` + carry-over; release gate: Set_0 non-empty before M is published |
| A14 | Governance key compromise (if a multisig bootstrap is allowed) | prover swap / re-bootstrap / asset-table abuse | scope minimal (§5 of rev.1 governance): recommend validator-set-signed governance messages only; timelock; vault caps |
| A15 | Ethereum-side bugs (reentrancy, overflow, precompile gas changes) | standard | checks-effects-interactions; `u256` amounts; precompile costs only affect gas, not correctness |
| A16 | Mass unstake after signing (nothing-at-stake for the epoch in force) | set in force keeps signing power until next snapshot | epoch length bounds it (one day); `MAX_CARRY_OVER` bounds persistence; V2 cooldown/slashing closes it economically |

---

## 9. Owner decisions (before any code)

| # | Item | Proposal |
|---|---|---|
| 1 | **F**: enforce at M in the v1 binary (adds `blst` to the release) vs pin F in the X-pin follow-up release | F = M if the v1 timeline absorbs ~3 days (blst + actions + snapshot + tests); else follow-up |
| 2 | `E` / `CHECKPOINT_INTERVAL` / `CHECKPOINT_DEPTH` | 1,440 / 60 / 20 |
| 3 | `MAX_FINALITY_SET`, `MIN_FINALITY_SET`, `MIN_FINALITY_WEIGHT`, `MAX_CARRY_OVER` | 8,192 / 4 / owner / 7 |
| 4 | Weight unit | whole modern B3 (`/1e9`) |
| 5 | Binding required for block eligibility from F (W_fin = W) | recommended yes, from F |
| 6 | `MAX_EPOCH_LAG` on Ethereum | 30 days |
| 7 | Successor-overlap rule (A4) | off |
| 8 | Ethereum governance model (validator-set-signed only vs timelocked multisig bootstrap) | owner |
| 9 | Vault caps / delay | owner |
| 10 | A3 height | later |

## 10. Unchanged

Deposits; M1 eligibility/seed; M3 block signature and wire format; M4; M5 fork choice
and horizon; the STAKE v1 carrier; FlowMesh D-3; everything ≤ H.

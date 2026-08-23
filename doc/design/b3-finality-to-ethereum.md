# B3 finality → Ethereum: the communication layer (design)

**Status: committed design record (2026-08-23), written to the owner's brief of the same
day ("solve the B3 consensus → Ethereum communication problem; do not redesign
deposits"). Direction: B3 Modern PoS produces BLS12-381 aggregate finality certificates
with a public signer bitmap — never 3,500 individual signatures, never MPC/TSS with hidden
signers; Ethereum verifies them in `B3FinalityVerifier.sol`; the vault releases ERC-20 only
after finality + withdrawal inclusion; the same data structures must later accept a ZK
proof of B3 finality instead of the BLS certificate.**

**Authority note.** The frozen Modern PoS V1 spec
([b3-modern-pos-spec.md](b3-modern-pos-spec.md), M1, §10) has *no epochs, no committees,
no finality gadget* and says none may be implemented without a new owner ruling. This
document designs that V2 layer **on top of** V1 — V1 eligibility, timestamps, block
signature (M3), fork choice (M5) and the reorg horizon are untouched — and is
**design only** until the owner rules it in. Deposits (Ethereum → B3 via `B3Bridge.sol`
state/receipt proofs) are as in [b3-bridge-bls-proposal.md](b3-bridge-bls-proposal.md)
§2.2 and are not changed here. Activation is A3, after the v1 release.

---

## 0. The problem in one picture

```
 B3 (Modern PoS V1 chain)                                Ethereum L1
 ─────────────────────────                                ───────────
 STAKE registry ──snapshot──► ValidatorSet_e ──hash──┐
 (validator_key, w, BLS pk)                          │
                                                     ▼
 checkpoint block ──► FinalizedBlock ──sign(BLS)──► Certificate ──relay──► B3FinalityVerifier
                      {height, hash,                {bitmap,                 • knows ValidatorSet_e
                       withdrawal_root,              agg_sig}                • checks quorum(weight)
                       validator_set_hash,                                   • checks agg BLS sig
                       epoch}                                                • learns ValidatorSet_e+1
                                                                             • stores withdrawal_root
 BRIDGE_BURN ──► cumulative withdrawal tree ──proof──────────────────────► B3Bridge.release()
                                                                             (finality ∧ inclusion)
```

Three objects cross the boundary and **nothing else**: `FinalizedBlock`, `ValidatorSet`
(by header + Merkle members), `Certificate`. A ZK prover later replaces only the
`Certificate` *proof bytes*, never the objects.

---

## 1. B3 side — the finality gadget (V2 layer on V1)

### 1.1 Validator identity and BLS key binding

- A validator is a `validator_key` (32 B, BIP340 x-only per M3) with ACTIVE weight `w`
  from the STAKE registry (`src/node/stake_tracker`).
- **`VALIDATOR_BLS_BINDING` action** (new creation-action type, activation A3):
  `{validator_key 32, bls_pubkey 48, pop 96, bip340_sig 64}` where `pop` is the BLS
  proof-of-possession (signature over `bls_pubkey` under the POP DST) and `bip340_sig`
  is the validator key's signature over `TaggedHash("B3/FINALITY/BIND/V1", ModernChainDomain
  ‖ validator_key ‖ bls_pubkey)`. Binding is state (latest wins, undo on disconnect);
  rebinding is allowed, and takes effect at the next snapshot. STAKE v1's 32 + 2 B
  carrier is **not** changed.
- Validators **without** a binding are excluded from the finality set (their weight does
  not count in `W_fin`); they still produce blocks under V1. This keeps liveness in the
  hands of those who opted in and makes "join the finality set" an explicit act.

### 1.2 Epochs and the set snapshot (lookahead = 1)

- `FINALITY_EPOCH_BLOCKS = E` (owner; recommend **1440** = one day = the ratified reorg
  horizon). Epoch `e` covers heights `[A3 + e·E, A3 + (e+1)·E)`.
- **`ValidatorSet_{e+1}` is snapshotted at the last block of epoch `e−1`** (ACTIVE
  weight with the 20-block maturity already in V1, bound BLS key present, `w > 0`),
  sorted by `validator_key`, indexed `0..n−1`. It is therefore known for the whole of
  epoch `e`, and the set that signs in epoch `e` (`ValidatorSet_e`) was known for the
  whole of `e−1`. This is the sync-committee handover pattern: **the set in force always
  attests the successor set.**
- Set header (all integers big-endian fixed width; hashes are **keccak-256** because
  Ethereum will recompute them):

```
ValidatorSetHeader {
  epoch             u64
  ruleset_version   u16    // quorum rule id; changes only by B3 consensus upgrade
  validator_count   u32    // n ≤ MAX_FINALITY_SET (owner; 8192 proposed)
  total_weight      u64    // W_fin = Σ w_i  (in modern base units, or scaled — see §6)
  quorum_weight     u64    // the exact threshold Ethereum must check: ceil(2·W_fin/3)+1 (ruleset 1)
  aggregate_pubkey  48 B   // Σ_i bls_pubkey_i in G1 (unweighted), computed by B3 consensus
  members_root      32 B   // keccak Merkle root over leaves, padded to 2^⌈log2 n⌉ with zero-leaves
}
leaf_i           = keccak( uint32 index_i ‖ bls_pubkey_i(48) ‖ uint64 weight_i )
validator_set_hash = keccak( ValidatorSetHeader )
```

`quorum_weight` is stored explicitly so that **quorum changes** are just a new
`ruleset_version` + number inside a signed header — Ethereum never hard-codes the rule.

### 1.3 What gets finalized and how it is signed

- **Checkpoint candidates**: every block at height ≡ 0 mod `CHECKPOINT_INTERVAL` (owner;
  recommend **60** = one hour at 60 s spacing; may be lowered to 1 for the DEX later —
  the contract design below does not care) that is ≥ `CHECKPOINT_DEPTH` deep (recommend
  **20**, matching STAKE maturity) — a checkpoint is signed only once it is unlikely to
  be reorganized under V1 fork choice.
- **`FinalizedBlock`** (the owner's structure, one field added):

```
FinalizedBlock {
  height              u64
  block_hash          32 B    // the modern block hash
  withdrawal_root     32 B    // cumulative withdrawal accumulator root as of this block (§3)
  validator_set_hash  32 B    // keccak(ValidatorSetHeader of ValidatorSet_{epoch+1})  — the successor set
  epoch               u64     // epoch of the SIGNING set (ValidatorSet_epoch)
}
finality_digest = TaggedHash("B3/FINALITY/V1", ModernChainDomain ‖ FinalizedBlock)
```

  `validator_set_hash` is **always the successor set** — constant across an epoch, so
  every checkpoint in epoch `e` is a valid handover message; no special "last block of
  epoch" certificate exists and a relayer may skip checkpoints freely (§2.3).
- Every bound validator signs `finality_digest` with BLS (`BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_POP_`;
  pubkeys G1, signatures G2 — the Ethereum variant, so EIP-2537 verifies it natively)
  and gossips `finsig {epoch, height, index, sig}` on P2P (new message, bounded, per-
  validator-per-checkpoint deduped).
- **`Certificate`** = `{signer_bitmap: bitvector[n], aggregate_signature: 96 B}`.
  Anyone aggregates (plain G2 addition). Bitmap index = set index, so signer identity is
  public and accountable; a conflicting-checkpoint double-sign is provable evidence.
- Quorum: `Σ_{i ∈ bitmap} w_i ≥ quorum_weight`.

### 1.4 Where the certificate lives on B3 (consensus)

- A modern block may carry **one `FinalityCertificate` section** `{FinalizedBlock,
  Certificate}` for a checkpoint that is an ancestor of the block, has not yet been
  certified on this chain, and whose `epoch` is the current or previous one. Consensus
  verifies set membership (the node holds the snapshot), quorum, and the aggregate
  signature (`blst`). Invalid certificate ⇒ invalid block.
- **Finality pin (required for bridge safety):** once a block carrying a valid
  certificate for checkpoint `C` is connected, a reorganization that would disconnect
  `C` is refused (`modern-finality-violation`, no peer penalty — same posture as the
  horizon, but absolute). Without this rule Ethereum could release funds for a burn that
  B3 later reverts; **finality must be at least as strong on B3 as on Ethereum.** Honest
  nodes holding ≥ 2/3 of finality weight never sign two conflicting checkpoints at the
  same height, so no honest node ever pins two conflicting chains.
- Liveness failure (< quorum online) stalls certificates, never the chain: blocks keep
  coming under V1; withdrawals wait. Safe by construction.
- V1 has no slashing; certificates are **accountable** (bitmap) but not yet punished.
  Equivocation evidence (two certified conflicting checkpoints) is recorded for the
  V2 slashing rule (spec §10) — reported, not designed here.

### 1.5 Relayer-facing RPC (node, not consensus)

`getfinalityset <epoch>` (header + members), `getfinalitycertificate <height>`,
`getwithdrawalproof <withdrawal_id>`, `getfinalizedtip`. Everything a relayer submits is
public chain data; relaying is permissionless.

---

## 2. Ethereum side — `B3FinalityVerifier.sol`

### 2.1 State

```solidity
struct SetHeader { uint64 epoch; uint16 rulesetVersion; uint32 validatorCount;
                   uint64 totalWeight; uint64 quorumWeight;
                   bytes aggregatePubkey /*48*/; bytes32 membersRoot; }
struct FinalizedBlock { uint64 height; bytes32 blockHash; bytes32 withdrawalRoot;
                        bytes32 validatorSetHash; uint64 epoch; }

bytes32 chainDomain;            // B3 ModernChainDomain, immutable
uint64  currentEpoch;           // epoch whose set is in force
SetHeader currentSet;  bytes32 currentSetHash;
SetHeader nextSet;     bytes32 nextSetHash;   // learned from any certificate of currentEpoch; zero until then
FinalizedBlock latest;          // monotone in height
IB3FinalityProver prover;       // §4
```

### 2.2 Genesis (trusted bootstrap)

Constructor pins `chainDomain`, `currentEpoch = e0`, and the **full `SetHeader` of
`ValidatorSet_{e0}`** (its hash is the first `validator_set_hash`). That header is
published with the B3 release that activates A3 and is reproducible by any B3 node
(`getfinalityset e0`). This is the one trust root, identical in kind to a light client's
checkpoint; re-bootstrap after a verifier stall is a governance action (§5).

### 2.3 `submitCertificate` — the state machine

```
submitCertificate(FinalizedBlock fb, SetHeader successor, bytes proof)
  require fb.epoch == currentEpoch || (fb.epoch == currentEpoch + 1 && nextSetHash != 0)
  if fb.epoch == currentEpoch + 1:            // rotation — driven by the successor set signing
      currentSet = nextSet; currentSetHash = nextSetHash; currentEpoch += 1; nextSet = ∅
  require fb.height > latest.height           // monotone; equal-height conflicts impossible past the first
  require keccak(successor) == fb.validatorSetHash
  require prover.verify(chainDomain, fb, currentSetHash, currentSet, proof)   // §4 — BLS today
  if nextSetHash == 0: nextSet = successor; nextSetHash = fb.validatorSetHash
  else require nextSetHash == fb.validatorSetHash                             // consistency within an epoch
  latest = fb;  emit Finalized(fb)
```

- **Skipping** is free: any checkpoint of the current epoch is acceptable; only the
  epoch sequence is strict (`e → e+1`), because each set attests only its successor.
  A relayer must therefore land **≥ 1 certificate per epoch** (one per day at E = 1440)
  — that is the whole keep-alive cost; withdrawals ride on whatever checkpoint is latest.
- **Lag is recoverable**: B3 keeps every certificate in-block forever, so a verifier
  that fell behind replays one certificate per missed epoch in order (`e`, `e+1`, …) and
  catches up. Only a *B3-side* liveness failure — no quorum certificate at all for an
  entire epoch — breaks the chain of custody; that is the re-bootstrap case (§5).
- **Quorum changes / epoch transitions** are nothing special: they arrive inside a signed
  successor header.

### 2.4 `BlsCertificateProver` — what `proof` contains today

`proof = abi.encode(bytes signerBitmap, bytes aggregateSignature /*96*/,
                    NonSigner[] nonSigners, bytes32[] multiproof)`
with `NonSigner {uint32 index; bytes pubkey /*48*/; uint64 weight;}`.

Verification (all via EIP-2537 precompiles — live on L1):

1. `popcount(bitmap) + nonSigners.length == validatorCount`; every non-signer index is a
   0-bit; indices strictly increasing; bits beyond `n` are zero.
2. Multiproof of the non-signer leaves against `membersRoot`.
3. `signedWeight = totalWeight − Σ nonSigner.weight ≥ quorumWeight`.
4. `aggPk = aggregatePubkey − Σ nonSigner.pubkey` (G1ADD with negated points) — the
   **subtraction trick**: cost scales with *absentees*, not with set size; at 3,500
   validators and 95 % participation that is 175 G1 adds, not 3,325.
5. `Hm = hash_to_curve_G2(finality_digest)` — `expand_message_xmd` with the SHA-256
   precompile, two `MAP_FP2_TO_G2`, one `G2ADD`; `finality_digest` recomputed from `fb`
   and `chainDomain` with the B3 tagged-hash convention (SHA-256, in-contract).
6. Pairing check `e(aggPk, Hm) · e(−G1, sig) == 1` (`BLS12_PAIRING_CHECK`, 2 pairs).

Gas (EIP-2537 prices; order of magnitude): pairing ≈ 103 k; hash-to-G2 ≈ 70 k; per
non-signer ≈ 375 (G1ADD) + multiproof share + ~1.2 k calldata ≈ 2–3 k. **3,500
validators, 95 % participation ≈ 0.6–0.8 M gas; 99 % ≈ 0.3 M; 80 % ≈ 2 M.** One
certificate per day keeps the verifier alive; one per withdrawal batch serves users.
(Storing the full set on-chain — 3,500 × 48 B ≈ 100 M gas — is why the set is
committed by header + Merkle, and why the aggregate pubkey is carried in the header.)

### 2.5 Read API used by the vault

`latestFinalized()`, `isFinalized(height, hash)`, `withdrawalRoot()`.

---

## 3. Withdrawals — accumulator, proof, release

- `BRIDGE_BURN` (from the bridge proposal) destroys units of a `BRIDGE_BACKED` asset and
  names `{origin_chain_id, origin_token(20), recipient(20), amount}`.
- B3 chainstate maintains **one cumulative incremental Merkle tree per origin chain**
  (depth 32, keccak, the Ethereum deposit-contract construction; append-only, undo on
  disconnect): `leaf = keccak(uint64 withdrawal_id ‖ bytes32 asset_id ‖ address origin_token
  ‖ address recipient ‖ uint256 amount ‖ uint64 b3_height)`. Its root at the checkpoint is
  `FinalizedBlock.withdrawal_root`. **Cumulative** means any older burn is provable against
  any newer root, so Ethereum stores only the latest.
- `B3Bridge.release(Withdrawal w, bytes32[32] path)`:
  `require !released[w.id]; require verifier.latestFinalized().withdrawalRoot
  contains leaf(w) at index w.id; require tokenOf[w.asset_id] == w.origin_token;
  released[w.id] = true; IERC20(w.origin_token).transfer(w.recipient, w.amount)`.
  Optional owner-ruled rate limit / delay window sits here, not in the verifier.
- **Release happens only after (a) a finality certificate covering a root that
  (b) includes the burn.** No signer set, admin, or relayer can release otherwise.

---

## 4. ZK-forward compatibility — freeze the objects, swap the prover

```solidity
interface IB3FinalityProver {
    function verify(bytes32 chainDomain, FinalizedBlock calldata fb,
                    bytes32 setHash, SetHeader calldata set, bytes calldata proof)
        external view returns (bool);
}
```

- **Today**: `BlsCertificateProver` — `proof` = bitmap + aggregate signature + non-signer
  data (§2.4).
- **Future**: `ZkFinalityProver` — `proof` = a SNARK/STARK whose public inputs are exactly
  `(chainDomain, keccak(fb), setHash)` and whose circuit verifies *the same BLS
  certificate against the same set commitment* (or, later, verifies B3 block headers
  themselves). **Nothing on B3 changes**: validators keep signing, bitmaps stay public and
  accountable; only Ethereum's verification cost collapses to one proof verify.
- Frozen forever (version-tagged `V1`): `FinalizedBlock`, `ValidatorSetHeader`/leaf
  encoding, `Certificate`, `finality_digest` domain, withdrawal leaf and tree. A new
  `ruleset_version` changes numbers, never layouts.
- Prover replacement = the one governance-sensitive mutation (§5).

---

## 5. Governance surface — explicitly small

Only three things can ever be changed on the Ethereum side: the **prover address**, a
**re-bootstrap** (new `SetHeader` after a B3-side finality stall), and the **asset
table** in the vault (B3 `asset_id` ↔ ERC-20). Recommended: no EOA admin; these are
executed only on a **`B3GovernanceMessage`** — a `FinalizedBlock`-shaped message with a
distinct domain (`B3/FINALITY/GOV/V1`) signed by the current validator set under the same
quorum, so the B3 validator set governs its own light client; bootstrapping from an
unreachable state needs an out-of-band social re-deploy exactly like any light client.
Whether a timelocked multisig is tolerated for the first months is an **owner decision**.

---

## 6. Parameters and open owner decisions

| # | Item | Proposal |
|---|---|---|
| 1 | Rule in the V2 finality gadget (epochs, snapshot, certificate section, finality pin) | required before any code |
| 2 | `FINALITY_EPOCH_BLOCKS` E | 1440 (one day = reorg horizon) |
| 3 | `CHECKPOINT_INTERVAL` / `CHECKPOINT_DEPTH` | 60 / 20 |
| 4 | Quorum rule (ruleset 1) | `ceil(2·W_fin/3)+1` by ACTIVE weight; `MAX_FINALITY_SET` 8192 |
| 5 | Weight units in the header | `w / 1e9` (whole modern B3) to keep `u64` comfortable — confirm |
| 6 | Unbound validators: excluded from `W_fin` (proposed) vs counted as absent | excluded |
| 7 | Finality pin semantics on B3 (absolute refusal, proposed) | yes |
| 8 | Participation incentive / slashing | V2, after OD-2 reward ruling |
| 9 | Ethereum governance: validator-set-signed only vs timelocked multisig bootstrap | owner |
| 10 | Vault rate limit / delay | owner |
| 11 | Set-rotation and keep-alive relayer funding | owner |

---

## 7. What this does not change

- Deposits (Ethereum → B3) — unchanged from the bridge proposal.
- Modern PoS V1 block production, fork choice, horizon, STAKE v1 carrier, block hashing.
- FlowMesh D-3 (BIP340, no BLS) — the finality gadget is base-chain, not FlowMesh.
- Anything ≤ H.

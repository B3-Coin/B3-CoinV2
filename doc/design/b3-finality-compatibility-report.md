# Modern PoS V1 + Cross-Chain Finality — Compatibility Report

**Date 2026-08-23. Scope: protocol-correctness review of
[b3-modern-pos-spec.md](b3-modern-pos-spec.md) (V1 + M7),
[b3-cross-chain-finality-v1.md](b3-cross-chain-finality-v1.md) (normative),
OD-8 in [b3-open-decisions.md](b3-open-decisions.md), and the consensus rules in the tree
(`src/modern/*`, `src/consensus/*`, `src/validation.cpp`, `src/node/stake_tracker.*`).
Goal: B3 consensus born compatible with BLS finality and future Ethereum verification.
No code, no implementation; the bridge is not redesigned and no bridge mechanism is added.
Findings marked ⚠ require an owner ruling; ✔ = confirmed consistent; ✎ = editorial.**

---

## 0. Summary — three findings that change the plan, everything else confirms

| # | Finding | Consequence |
|---|---|---|
| **F-1 ⚠** | The normative spec places the certificate and the BLS binding in "creation actions type 4/5". **The creation-action section has no live consensus carrier in the tree**: `ModernTransition`/`ModernTransitionV2` are models with "NO generic serialization on purpose", used only in tests; `validation.cpp` has no creation-action hook; status row 147 confirms "nothing decodes them from a peer". The only live modern carriers are **script-level** (`B3S1` STAKE push, the M3 coinbase `scriptSig` declaration, `vchBlockSig`). | Pick the v1 carrier now (§1). Recommended: script-level magic carriers in the txid-committed part of the block, mirroring the ratified STAKE pattern; keep creation-action numbers 4/5 RESERVED for the future codec. |
| **F-2 ⚠** | Spec §4 rotates validator sets on **fixed heights**; the owner's requirement is rotation **only after the current set has produced a valid transition certificate**. As written, a set can take over without `Set_e` ever certifying, which breaks Ethereum's lineage on a one-epoch liveness lapse. | Amend to **handover-gated rotation** (§3). Dissolves the lineage-break case; B3 then matches the verifier's own rule exactly. |
| **F-3 ✔ (resolves the F decision)** | The v1 binary ships with **H set, X blank and `ModernPosParams` unset**, so it refuses H+1 and can never reach M; the mandatory **X-pin follow-up release** is the binary that runs the corridor and Modern PoS. | **F = M is achievable without putting `blst` into the v1 binary**: the finality rules, `blst`, and the finality parameters ride in the X-pin release together with the PoS parameters. No reservation is needed in v1 at all — v1 processes no modern-era block. Only constraint: the finality implementation must be complete before the X-pin release is cut (days after block H). |

---

## 1. Block structure

| Question | Answer | Status |
|---|---|---|
| Where exactly does `FINALITY_CERTIFICATE` live? | **Recommended v1 carrier:** a zero-value output of the **coinbase** with `scriptPubKey = OP_RETURN PUSH("B3F1" ‖ FinalizedBlock(120) ‖ bitmap(⌈n/8⌉) ‖ sig(96))`, minimal push, **at most one such output per coinbase**, in any modern-PoS block. It cannot live in the coinbase `scriptSig` (consensus cap 100 B, `bad-cb-length`, and M3 already defines that field) and must not live in `vchBlockSig` (trailing, **not committed by the block hash**). OP_RETURN data is never executed, so the 520-byte element limit does not apply; size is bounded by the block. | ⚠ ruling (F-1); magic `B3F1` to freeze |
| Where does the BLS binding live? | Zero-value output `OP_RETURN PUSH("B3B1" ‖ validator_key ‖ bls_pubkey ‖ pop ‖ bip340_sig)` (240 B + magic) in any modern-era transaction from H+1. Recognition by magic + exact length, minimal push, exactly like `B3S1`; malformed claims are invalid, never ignored. | ⚠ ruling (F-1); magic `B3B1` to freeze |
| Is it consensus-critical? | Yes from F (= M): certificate quorum + BLS validity ⇒ block validity; binding well-formedness + PoP + BIP340 ⇒ transaction validity. | ✔ |
| Block hash before or after the certificate? | The certificate signs the **block_hash of an earlier checkpoint**; it is carried in a **later** block whose coinbase txid → merkle root → header hash commits to it, and the M3 block signature covers that hash. No circularity, no malleability of uncommitted data. | ✔ |
| Can Ethereum reconstruct the signed object? | Yes: `finality_digest = TaggedHash("B3/FINALITY/V1", ModernChainDomain ‖ FinalizedBlock)`; `ModernChainDomain = H("B3/MODERN/CHAIN" ‖ genesis ‖ X)` is a constant stored in the verifier; `FinalizedBlock` is 120 fixed-width bytes; `block_hash` = the modern block index hash (opaque to Ethereum). | ✔ — the modern block hash must be stated as `CBlockIndex::GetBlockHash()` (header hash), ✎ |
| Fixed-width / canonical? | All protocol fields fixed-width big-endian; bitmap length derived from `n`; minimal-push rule for carriers; no varints anywhere in signed objects. | ✔ (carrier encoding rule to be frozen) |
| Domain availability | `ModernChainDomain` requires X pinned; bindings from H+1 and certificates from M both occur only in the X-pinned binary. | ✔ |
| Relay | 240 B / ~1.3 KB OP_RETURN outputs exceed the 80-byte datacarrier **policy**; the binding transaction needs a standardness carve-in (as STAKE got); the coinbase never relays. Policy, not consensus. | ✎ engineering |

---

## 2. Validator key model

| | BIP340 `validator_key` | BLS12-381 `bls_pubkey` |
|---|---|---|
| Identity | the validator's consensus identity: carried in every STAKE output (`B3S1`, 32 B opaque, interpreted as x-only by M3 — confirmed `pos_v1.h:173`), aggregated per key | none by itself; always bound **to** a `validator_key` |
| Ownership / control | **funds** are controlled by the STAKE output's owner-script suffix (spend = unstake); **block production** is controlled by whoever holds the validator secret — these may differ (operator model) | held by the validator operator; binding is authorized by the `validator_key` (BIP340 signature), not by the fund owner |
| Authorizes | blocks (M3), bindings (`B3/FINALITY/BIND/V1`) | finality digests only (`B3/FINALITY/V1`), its own PoP; later any cross-chain proof of the same certificates |
| Derivation | independent secrets; **never derived from each other** (spec §1) | ✔ |
| Rotation | re-stake (M4, 20-block maturity) | rebind (latest wins); effective at the **next snapshot** — with snapshot-at-epoch-start (§3) a rebind in epoch `e` joins `Set_{e+2}` (≤ 2 epochs lag) | ✔, lag to document ✎ |
| Proof of possession | mandatory in the binding (PoP DST); one BLS key bound to one validator at a time | ✔ |
| Activation timing | binding valid from H+1 (corridor); `Set_0 = Snapshot(M−1)`; STAKE maturity (20) and binding must both hold at the snapshot | ✔ |
| Note ⚠ | Because binding authority = validator secret (operator), finality voting power follows the block-production operator, not the fund owner — same trust boundary as block production. Confirm this is intended. | ⚠ confirm |

---

## 3. Epoch and validator-set transition (F-2 amendment, proposed text)

```
Set_e  ──(certificate of epoch e: carries hash(Set_{e+1}))──►  rotation  ──►  Set_{e+1}
```

Proposed normative rule (replaces spec §4 "Epoch/Snapshot" rows):

- `epoch_start[0] = M`. At the first block of epoch `e`, `Set_{e+1} := Snapshot(epoch_start[e] − 1)`
  (so `Set_1 = Set_0 = Snapshot(M−1)`). `Set_{e+1}` is therefore known for all of epoch `e`
  and every epoch-`e` certificate carries `hash(Set_{e+1})`.
- **Epoch `e+1` begins at the first height `h` such that `h ≥ epoch_start[e] + E` AND the
  chain below `h` contains a valid certificate with `epoch = e`.** Until then epoch `e`
  **extends** (checkpoints continue every `CHECKPOINT_INTERVAL`, signed by `Set_e`, all
  with `epoch = e`).
- Carry-over (undersized snapshot ⇒ `Set_{e+1} = Set_e` re-stamped) unchanged.
- `MAX_EPOCH_EXTENSION` (7·E proposed): an epoch extended beyond it declares the lineage
  broken (no further certificate valid; consensus re-bootstrap rule). Replaces
  `MAX_CARRY_OVER` as the stale-set bound.

Confirmations under the amended rule:

| Property | Result |
|---|---|
| Old set authorizes next set | On Ethereum: always (only certificates of `Set_e` disclose `Set_{e+1}`). On B3: `Set_{e+1}` is *derived* from chain state by V1 consensus and *attested* by `Set_e`'s certificate; it can never become the signing set before that attestation exists on-chain. ✔ |
| No set appears without previous authority | Ethereum side: structurally impossible (`fb.epoch == current+1` requires `nextSetHash != 0`). B3 side: gated rotation gives the same property. ✔ after F-2 |
| Rotation only AFTER a valid transition certificate | ✔ after F-2 (was ✘ in the fixed-height text) |
| Long-range | Ethereum: `MAX_EPOCH_LAG` on rotation; B3: `MAX_EPOCH_EXTENSION`, finality pin, reorg horizon 1,440; snapshot block is an ancestor of the certified checkpoint, so the set definition is itself finalized by the certificate. ✔ Residual (A2/A3 in the design record): > 2/3 weight or a stale verifier beyond the lag bound — the assumption, not a gap. |
| Ethereum follows the same chain of transitions | Identical rule on both sides: `e → e+1` strictly, any checkpoint inside an epoch, successor disclosed by the first certificate of `e`. ✔ |

---

## 4. Finality gadget — exact rules (confirmed / amended)

| Rule | Specification |
|---|---|
| Finalized object | `FinalizedBlock{height, block_hash, withdrawal_root, validator_set_hash(successor), epoch}` of a checkpoint block (`(h−M) mod CHECKPOINT_INTERVAL = 0`, depth ≥ `CHECKPOINT_DEPTH`; plus every block that ends an epoch under §3). |
| Quorum | `signed_weight ≥ floor(2W/3) + 1` (ruleset 1); `W` and all `w_i` in whole modern B3 (`/10^9`), u64; min stake 333 ⇒ `w_i ≥ 333`. ✔ |
| Stake weighting | `w_i` = ACTIVE principal per `validator_key` at the snapshot height (`StakeTracker::ActiveWeight` semantics; 20-block maturity); `w_i = 0` excluded; head-count irrelevant. ✔ (engineering: the tracker exposes per-key weight; a **full-set enumeration at a height** is required for `Snapshot` — not a protocol change) |
| Certificate validation order | 1 carrier syntax (coinbase, ≤ 1, magic, minimal push, exact length for the epoch's `n`) → 2 `epoch ∈ {current, current−1}` and checkpoint height inside that epoch's range and `> finalized_tip.height` → 3 checkpoint `block_hash` is the ancestor at that height on this chain (block index, cheap) → 4 bitmap rules → 5 quorum by weight → 6 `FastAggregateVerify` (`blst`, last, expensive) → 7 on connect: `finalized_tip = checkpoint`. Failures: `bad-finality-cert-form` / `bad-finality-cert`; block invalid; no peer penalty beyond the usual invalid-block handling. ⚠ (the `{current, current−1}` window is new — needed so a higher epoch-`e` checkpoint can still be certified in the first blocks of `e+1`) |
| Reorg after finality | From F: a reorganization that would disconnect `finalized_tip` is refused (`modern-finality-violation`), no peer penalty, skipped during reindex/import; horizon (1,440) and `AbandonOffAnchorTip` semantics unchanged. Pins apply only on the active chain at connect time; side-chain certificates are validated in their own chain context but never pin. ✔ |
| Validators fail to certify | Chain continues under V1 (blocks do not depend on certificates); epoch extends (§3); withdrawals wait; past `MAX_EPOCH_EXTENSION` the lineage is broken → consensus re-bootstrap release. No loss, no halt of B3. ✔ |
| Signing discipline (node) | one signature per height, increasing heights, descendants of the latest certified checkpoint only; `finsig` relay bounded. ✔ |

---

## 5. Ethereum verification model — confirmed

| Stores | genesis `SetHeader` + hash (contains the genesis `members_root`), `setHashByEpoch[e]` (epoch headers by hash), `currentSetHash` + header, `nextSetHash` + header, `latest{height, hash, withdrawalRoot}`, `lastRotationTime`, `prover`. **No member list.** ✔ |
|---|---|
| `members_root` proof model | fixed depth 13 (8,192), leaf `keccak(u32 i ‖ pk48 ‖ u64 w)`, zero-leaf padding; absentees proven by OZ-style multiproof; membership + weights come only from proofs + the signed header. ✔ |
| Signer bitmap | `⌈n/8⌉` bytes, LSB-first, high bits zero, absentee bits zero, `popcount + |absent| = n`. ✔ |
| Aggregate BLS | `aggPk = aggregate_pubkey − Σ absent` (G1ADD of negations), `Hm = hash_to_G2(digest)`, pairing `e(aggPk,Hm)·e(−G1,sig)=1` via EIP-2537; `aggregate_pubkey` is consensus-computed on B3 and attested by the signing quorum. ✔ |
| Quorum | `signedWeight ≥ quorumWeight` and `quorumWeight == (2W)/3 + 1` enforced on-chain for ruleset 1; unknown ruleset rejected. ✔ |
| Withdrawal root | single cumulative depth-32 keccak tree, leaf with `origin_chain_id`, index = `withdrawal_id`; release = path to `latest.withdrawalRoot` ∧ chain id ∧ unreleased ∧ asset table. ✔ (all-zero root before A3) |
| Epoch transition | `fb.epoch == current` or `current+1` with `nextSetHash != 0`; rotation before verification; successor header hash-checked and rule-checked. Matches B3 §3 after F-2. ✔ |

---

## 6. Future ZK compatibility — confirmed (ZK deferred by owner)

`IB3FinalityProver.verify(chainDomain, fb, setHash, set, proof)`; ZK public inputs
`(chainDomain, keccak(fb), setHash)`, statement = spec §5.3 A–F. Unchanged by a prover
swap: `FinalizedBlock` (120 B), withdrawal tree (depth 32, leaf, index), asset/bridge
model, B3 signing and carriers, verifier state machine. ✔

---

## 7. Contradictions and required decisions

### 7.1 Conflicts with existing design (report, not resolved)

| Where | Conflict | Proposed resolution (owner) |
|---|---|---|
| OD-8 text | "release leg … until then runs through a rotatable signer set or an optimistic scheme; the bridged-asset policy carries a rotatable `signer_set`" | **Superseded** by the finality protocol (no signer set anywhere). Strike by ruling. |
| `b3-bridge-bls-proposal.md` §2.2 item 5 / §2.3 | "each block commits a bridge exit root"; "`B3LightClient.sol` stores the B3 committee (pubkeys)" | Superseded: cumulative `withdrawal_root` inside `FinalizedBlock`; verifier stores roots, never pubkeys. Mark superseded. |
| `b3-cross-chain-finality-v1.md` §1/§4 | "creation action type 4/5" | F-1: carriers are script-level in v1; reserve 4/5 in the registry, never reuse. Amend after ruling. |
| `b3-cross-chain-finality-v1.md` §4 | fixed-height epochs | F-2: handover-gated rotation. Amend after ruling. |
| `b3-finality-to-ethereum.md` §0 | "F pinned by the follow-up release if v1 cannot carry `blst`" framed as a trade-off | F-3: v1 never reaches M; the X-pin release is the natural and only home for F = M. Editorial. |
| `b3-modern-pos-spec.md` §9 | parameter table lacks the finality rows | add `finality_epoch_blocks`, `checkpoint_interval`, `checkpoint_depth`, `min_finality_set`, `min_finality_weight`, `max_epoch_extension` to `ModernPosParams` with the same **unset ⇒ fail-closed** semantics; mainnet stays unset until the X-pin release. |
| M6 / coinbase rules | none: a zero-value OP_RETURN coinbase output violates no modern rule; `GetValueOut` unaffected | ✔ |
| M3 wire format | none: OP_RETURN carriers use existing transaction structure; `vchBlockSig` untouched | ✔ |
| Architecture contract (policy outputs / creation actions as the modern data model) | v1 STAKE already set the precedent of a script-level carrier by ratified ruling; finality carriers follow it | ✔ by precedent; record in M7 |

### 7.2 Required PoS decisions (before coding)

1. **F-1 carrier ruling**: `B3F1` coinbase OP_RETURN certificate + `B3B1` binding output (recommended) — or wait for the modern codec (not available for v1).
2. **F-2 handover-gated rotation** + `MAX_EPOCH_EXTENSION` (7·E) replacing `MAX_CARRY_OVER`.
3. **F = M in the X-pin release** (confirm; no `blst` in v1).
4. Certificate epoch window `{current, current−1}`.
5. Binding required for block eligibility from F (W_fin = W) — yes/no.
6. Binding authority = validator secret (operator) — confirm.
7. Parameters: `E` 1,440; `CHECKPOINT_INTERVAL`/`DEPTH` 60/20; `MIN_FINALITY_SET` 4; `MIN_FINALITY_WEIGHT`; `MAX_EPOCH_LAG` 30 d (Ethereum); standardness carve-in for `B3B1`.

### 7.3 Fields frozen before coding (byte-exact)

Tags `"B3/FINALITY/V1"`, `"B3/FINALITY/BIND/V1"`; carrier magics `B3F1`, `B3B1` + minimal-push
rule; `ValidatorSetHeader` 126 B; leaf preimage 60 B; `SET_TREE_DEPTH` 13; `FinalizedBlock`
120 B; bitmap LSB-first; certificate = `fb ‖ bitmap ‖ sig96`; binding payload 240 B;
ciphersuite + DSTs; `ModernChainDomain` definition; quorum `floor(2W/3)+1`; weight unit
`/10^9`; withdrawal leaf 164 B, depth 32, zero-hash chain; `Set_0 = Snapshot(M−1)`;
snapshot-at-epoch-start; gated rotation; checkpoint rule; `finality_digest`.

---

## 8. Verdict

With F-1 (carrier), F-2 (gated rotation) and the parameter rows added, Modern PoS V1 is
**born compatible** with BLS finality and Ethereum verification: the signed object is
deterministic and fixed-width, committed by the block hash, produced by a quorum of the
stake-weighted set whose lineage starts at `Snapshot(M−1)`, attested set-by-set, verifiable
on Ethereum from roots alone, and replaceable by a ZK prover without touching any
structure. No bridge mechanism was added or changed.

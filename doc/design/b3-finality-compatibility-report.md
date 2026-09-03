# Modern PoS V1 + Cross-Chain Finality — Compatibility Report

**Date 2026-08-23. Scope: protocol-correctness review of
[b3-modern-pos-spec.md](b3-modern-pos-spec.md) (V1 + M7),
[b3-cross-chain-finality-v1.md](b3-cross-chain-finality-v1.md) (normative),
OD-8 in [b3-open-decisions.md](b3-open-decisions.md), and the consensus rules in the tree
(`src/modern/*`, `src/consensus/*`, `src/validation.cpp`, `src/node/stake_tracker.*`).
Goal: B3 consensus born compatible with BLS finality and future Ethereum verification.
This is a historical 2026-08-23 audit. The 2026-09-02 implementation amendment
replaces its OpenZeppelin multiproof realization with index-sensitive ordered
depth-13 paths and performs epoch rotation only after successor-set signature
verification. Findings marked ⚠ require an owner ruling; ✔ = confirmed
consistent as of the original audit; ✎ = editorial.**

---

## 0. Summary — three findings that change the plan, everything else confirms

| # | Finding | Consequence |
|---|---|---|
| **F-1 ⚠ (revised §7.4)** | The normative spec places the certificate and the BLS binding in "creation actions type 4/5". **The creation-action section has no live consensus carrier in the tree**: `ModernTransition`/`ModernTransitionV2` are models with "NO generic serialization on purpose", used only in tests; `validation.cpp` has no creation-action hook; status row 147 confirms "nothing decodes them from a peer". The only live modern carriers are **script-level** (`B3S1` STAKE push, the M3 coinbase `scriptSig` declaration, `vchBlockSig`). | **Revised in §7.4 after the owner's correction (modern era = Policy Outputs):** binding = `STAKE` policy **v2** (params `vk ‖ bls_pk` = 80 B, PoP verified at creation); certificate = new policy type **`FINALITY_CERT = 6`** (coinbase-only, amount 0, commitment = `finality_digest`, type-specific params bound). No OP_RETURN, no scriptSig, no `vchBlockSig`. Creation-action numbers 4/5 stay RESERVED. |
| **F-2 ⚠** | Spec §4 rotates validator sets on **fixed heights**; the owner's requirement is rotation **only after the current set has produced a valid transition certificate**. As written, a set can take over without `Set_e` ever certifying, which breaks Ethereum's lineage on a one-epoch liveness lapse. | Amend to **handover-gated rotation** (§3). Dissolves the lineage-break case; B3 then matches the verifier's own rule exactly. |
| **F-3 ✔ (resolves the F decision)** | The v1 binary ships with **H set, X blank and `ModernPosParams` unset**, so it refuses H+1 and can never reach M; the mandatory **X-pin follow-up release** is the binary that runs the corridor and Modern PoS. | **F = M is achievable without putting `blst` into the v1 binary**: the finality rules, `blst`, and the finality parameters ride in the X-pin release together with the PoS parameters. No reservation is needed in v1 at all — v1 processes no modern-era block. Only constraint: the finality implementation must be complete before the X-pin release is cut (days after block H). |

---

## 1. Block structure

| Question | Answer | Status |
|---|---|---|
| Where exactly does `FINALITY_CERTIFICATE` live? | **A `FINALITY_CERT` (type 6) Policy Output in the coinbase** — ≤ 1 per block, `amount = 0`, native asset, commitment = `finality_digest`, params = `FinalizedBlock ‖ bitmap ‖ sig` (type-specific bound ≤ 1,232 B); unspendable cell like `BURN`; v1 wire realization is the policy carrier for unspendable cells, recognized and validated as a typed policy, never as free data. Not the coinbase `scriptSig` (100-byte cap, M3's field), not `vchBlockSig` (not committed by the block hash). See §7.4. | ⚠ ruling (F-1 revised) |
| Where does the BLS binding live? | **In the STAKE output itself: `STAKE` policy v2**, params `validator_key(32) ‖ bls_pubkey(48)` (80 B), carrier `B3S2`, PoP verified at creation, valid from H+1 like v1. See §7.4. | ⚠ ruling (F-1 revised) |
| Is it consensus-critical? | Yes from F (= M): certificate quorum + BLS validity ⇒ block validity; binding well-formedness + PoP + BIP340 ⇒ transaction validity. | ✔ |
| Block hash before or after the certificate? | The certificate signs the **block_hash of an earlier checkpoint**; it is carried in a **later** block whose coinbase txid → merkle root → header hash commits to it, and the M3 block signature covers that hash. No circularity, no malleability of uncommitted data. | ✔ |
| Can Ethereum reconstruct the signed object? | Yes: `finality_digest = TaggedHash("B3/FINALITY/V1", ModernChainDomain ‖ FinalizedBlock)`; `ModernChainDomain = H("B3/MODERN/CHAIN" ‖ genesis ‖ X)` is a constant stored in the verifier; `FinalizedBlock` is 120 fixed-width bytes; `block_hash` = the modern block index hash (opaque to Ethereum). | ✔ — the modern block hash must be stated as `CBlockIndex::GetBlockHash()` (header hash), ✎ |
| Fixed-width / canonical? | All protocol fields fixed-width big-endian; bitmap length derived from `n`; minimal-push rule for carriers; no varints anywhere in signed objects. | ✔ (carrier encoding rule to be frozen) |
| Domain availability | `ModernChainDomain` requires X pinned; bindings from H+1 and certificates from M both occur only in the X-pinned binary. | ✔ |
| Relay | STAKE v2 outputs need the same standardness carve-in STAKE v1 got; the coinbase never relays. Policy, not consensus. | ✎ engineering |

---

## 2. Validator key model

| | BIP340 `validator_key` | BLS12-381 `bls_pubkey` |
|---|---|---|
| Identity | the validator's consensus identity: carried in every STAKE output (`B3S1`, 32 B opaque, interpreted as x-only by M3 — confirmed `pos_v1.h:173`), aggregated per key | none by itself; always bound **to** a `validator_key` |
| Ownership / control | **funds** are controlled by the STAKE output's owner-script suffix (spend = unstake); **block production** is controlled by whoever holds the validator secret — these may differ (operator model) | declared by the **staker** in the STAKE v2 output, exactly as the validator key is declared in v1 (§7.4); PoP proves the declared key is real |
| Authorizes | blocks (M3) | finality digests only (`B3/FINALITY/V1`), its own PoP; later any cross-chain proof of the same certificates |
| Derivation | independent secrets; **never derived from each other** (spec §1) | ✔ |
| Rotation | re-stake (M4, 20-block maturity) | re-stake as v2 with the new key (M4); effective at the **next snapshot** — with snapshot-at-epoch-start (§3) a change in epoch `e` joins `Set_{e+2}` (≤ 2 epochs lag) | ✔, lag to document ✎ |
| Proof of possession | mandatory in the STAKE v2 carrier (PoP DST), verified at creation; one finality key per validator key by the largest-weight rule (§7.4) | ✔ |
| Activation timing | STAKE v2 valid from H+1 (corridor); `Set_0 = Snapshot(M−1)` over ACTIVE v2 outputs (20-block maturity) | ✔ |
| Note | Finality voting power follows the BLS key the **staker** declares, exactly as block production follows the validator key the staker declares — one trust model, no new authority. | ✔ |

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
| Long-range | Ethereum: absolute `GENESIS_TIME` epoch windows, `MAX_EPOCH_LAG`, and at least `MIN_EPOCH_DURATION` between accepted rotations; B3: `MAX_EPOCH_EXTENSION`, finality pin, reorg horizon 1,440; snapshot block is an ancestor of the certified checkpoint, so the set definition is itself finalized by the certificate. Rapid backlog epoch-walking is blocked, but honest catch-up takes one interval per rotation. Residual (A2/A3): > 2/3 weight or historical keys maintaining a competing lineage in real time remains the ordinary weak-subjectivity assumption. ✔ amended 2026-09-02 |
| Ethereum follows the same chain of transitions | Identical rule on both sides: `e → e+1` strictly, any checkpoint inside an epoch, successor disclosed by the first certificate of `e`. ✔ |

---

## 4. Finality gadget — exact rules (confirmed / amended)

| Rule | Specification |
|---|---|
| Finalized object | `FinalizedBlock{height, block_hash, withdrawal_root, validator_set_hash(successor), epoch}` of a checkpoint block (`(h−M) mod CHECKPOINT_INTERVAL = 0`, depth ≥ `CHECKPOINT_DEPTH`; plus every block that ends an epoch under §3). |
| Quorum | `signed_weight ≥ floor(2W/3) + 1` **and** `signed_count ≥ floor(2n/3) + 1` (ruleset 1); `W` and all `w_i` are whole modern B3 (`/10^9`), u64; min stake 333 ⇒ `w_i ≥ 333`. ✔ amended 2026-09-02 |
| Stake weighting | `w_i` = ACTIVE principal per `validator_key` at the snapshot height (`StakeTracker::ActiveWeight` semantics; 20-block maturity); `w_i = 0` excluded. Stake weight and validator headcount are independent quorum gates; neither substitutes for the other. ✔ amended 2026-09-02 (engineering: the tracker exposes per-key weight; a **full-set enumeration at a height** is required for `Snapshot` — not a protocol change) |
| Certificate validation order | 1 carrier syntax (coinbase, ≤ 1, magic, minimal push, exact length for the epoch's `n`) → 2 `epoch ∈ {current, current−1}` and checkpoint height inside that epoch's range and `> finalized_tip.height` → 3 checkpoint `block_hash` is the ancestor at that height on this chain (block index, cheap) → 4 bitmap rules → 5 quorum by weight and headcount → 6 `FastAggregateVerify` (`blst`, last, expensive) → 7 on connect: `finalized_tip = checkpoint`. Failures: `bad-finality-cert-form` / `bad-finality-cert`; block invalid; no peer penalty beyond the usual invalid-block handling. ⚠ (the `{current, current−1}` window is new — needed so a higher epoch-`e` checkpoint can still be certified in the first blocks of `e+1`) |
| Reorg after finality | From F: a reorganization that would disconnect `finalized_tip` is refused (`modern-finality-violation`), no peer penalty, skipped during reindex/import; horizon (1,440) and `AbandonOffAnchorTip` semantics unchanged. Pins apply only on the active chain at connect time; side-chain certificates are validated in their own chain context but never pin. ✔ |
| Validators fail to certify | Chain continues under V1 (blocks do not depend on certificates); epoch extends (§3); withdrawals wait; past `MAX_EPOCH_EXTENSION` the lineage is broken → consensus re-bootstrap release. No loss, no halt of B3. ✔ |
| Signing discipline (node) | one signature per height, increasing heights, descendants of the latest certified checkpoint only; `finsig` relay bounded. ✔ |

---

## 5. Ethereum verification model — confirmed

| Stores | genesis `SetHeader` + hash (contains the genesis `members_root`), `setHashByEpoch[e]` (epoch headers by hash), `currentSetHash` + header, `nextSetHash` + header, `latest{height, hash, withdrawalRoot}`, `lastRotationTime`, `prover`. **No member list.** ✔ |
|---|---|
| `members_root` proof model | fixed depth 13 (8,192), leaf `keccak(u32 i ‖ pk48 ‖ u64 w)`, zero-leaf padding; the current implementation proves each absentee with an ordered 13-sibling path because node hashing is index-sensitive; membership + weights come only from proofs + the signed header. ✔ amended 2026-09-02 |
| Signer bitmap | `⌈n/8⌉` bytes, LSB-first, high bits zero, absentee bits zero, `popcount + |absent| = n`. ✔ |
| Aggregate BLS | `aggPk = aggregate_pubkey − Σ absent` (G1ADD of negations), `Hm = hash_to_G2(digest)`, pairing `e(aggPk,Hm)·e(−G1,sig)=1` via EIP-2537; `aggregate_pubkey` is consensus-computed on B3 and attested by the signing quorum. ✔ |
| Quorum | B3 and Ethereum both enforce `signedWeight ≥ quorumWeight`, `quorumWeight == (2W)/3 + 1`, and `signerCount ≥ floor(2n/3)+1` for ruleset 1; unknown ruleset rejected. ✔ aligned 2026-09-02 |
| Withdrawal root | single cumulative depth-32 keccak tree, leaf with `origin_chain_id`, index = `withdrawal_id`; release = path to `latest.withdrawalRoot` ∧ chain id ∧ unreleased ∧ asset table. ✔ (all-zero before inbound bridge activation B; from B it is the nonzero canonical empty/current tree root even while the separate burn height W is unset; A3 is unrelated) |
| Epoch transition | `fb.epoch == current` or `current+1` with `nextSetHash != 0`; the precommitted successor verifies first and rotation occurs only after proof acceptance; successor header is hash-checked and rule-checked. Matches B3 §3 after F-2. ✔ amended 2026-09-02 |
| Bridge gas ceiling | `MAX_BRIDGE_VALIDATORS=64`; 64 distinct PoP keys, 43 signers and 21 absent paths verify under post-Fusaka Osaka in 18,144 proof bytes / 18,724 submit calldata bytes / 5,513,351 gas for the complete successful verifier call. Conservative EIP-7623 calldata gives 6,283,311 total, below EIP-7825. Larger current/successor sets close `bridgeReady`. ✔ measured 2026-09-02 |

---

## 6. Future ZK compatibility — confirmed (ZK deferred by owner)

`IB3FinalityProver.verify(chainDomain, fb, setHash, set, proof)`; ZK public inputs
`(chainDomain, keccak(fb), setHash)`, statement = spec §5.3 A–F. Unchanged by a prover
swap: `FinalizedBlock` (112 B), withdrawal tree (depth 32, leaf, index), asset/bridge
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

1. **F-1 carrier ruling (final form §7.5)**: `FINALITY_CERT = 6` (coinbase metadata cell, commitment = digest, 16-B params, certificate as creation payload — R1 script-carrier push vs R2 witness/annex) + `FINALITY_KEY = 7` (identity-authorized binding log: BIP340 by `validator_key` + PoP, `seq`-ordered); STAKE stays v1; `MAX_POLICY_PARAMS_SIZE` untouched; explicit non-UTXO rule for metadata cells.
2. **F-2 handover-gated rotation** + `MAX_EPOCH_EXTENSION` (7·E) replacing `MAX_CARRY_OVER`.
3. **F = M in the X-pin release** (confirm; no `blst` in v1).
4. Certificate epoch window `{current, current−1}`.
5. Binding required for block eligibility from F (W_fin = W) — yes/no.
6. `FINALITY_KEY` binding semantics (§7.5): identity-authorized (`validator_key` BIP340 + PoP), `seq`-ordered, zero key = revoke, effective at next snapshot — confirm; and whether `SegwitHeight` is pinned for the modern era (pre-existing gap).
7. Parameters: `E` 1,440; `CHECKPOINT_INTERVAL`/`DEPTH` 60/20; `MIN_FINALITY_SET` 4; `MIN_FINALITY_WEIGHT`; `MAX_EPOCH_LAG` 30 d (Ethereum); standardness carve-in for STAKE v2.

### 7.3 Fields frozen before coding (byte-exact)

Tag `"B3/FINALITY/V1"`; policy `STAKE` v2 (carrier `B3S2`, params 80 B) and `FINALITY_CERT = 6`
(commitment = digest, params bound); `ValidatorSetHeader` 110 B; leaf preimage 60 B; `SET_TREE_DEPTH` 13; `FinalizedBlock`
112 B; bitmap LSB-first; certificate = `fb ‖ bitmap ‖ sig96`; binding payload 240 B;
binding = STAKE v2 carrier (no separate 240-B action); ciphersuite + DSTs; `ModernChainDomain` definition; quorum `floor(2W/3)+1`; weight unit
`/10^9`; withdrawal leaf 164 B, depth 32, zero-hash chain; `Set_0 = Snapshot(M−1)`;
snapshot-at-epoch-start; gated rotation; checkpoint rule; `finality_digest`.

---

## 7.4 F-1 revised after owner correction (2026-08-23): Policy Outputs, not OP_RETURN

The modern era's data model is the Policy Output (`ModernOutput{asset, amount, policy_type,
policy_version, commitment 32 B, params ≤ 80 B}`, fail-closed on unknown types). The
§1 "OP_RETURN carrier" wording is withdrawn. Inspection of `src/modern/policy.h`:

| Policy | Unspendable? | Params | Arbitrary data? |
|---|---|---|---|
| LEGACY_LOCK 0 / OWNER 1 / DEX_VAULT 3 / FN 5 | no | empty / 3–35 B / empty | no |
| BURN 2 | yes | **must be empty**, commitment **must be zero** | no |
| STAKE 4 | no | 34 B (validator_key ‖ reserved) — v1 wire carrier `B3S1 … OP_DROP <owner script>` | no |

**There is no modern equivalent of OP_RETURN** (by design, contract §23). `BURN` is the
only unspendable cell and carries no bytes; the 80-byte cap excludes a certificate from any
existing policy. Classification decides the carrier:

- **`VALIDATOR_BLS_BINDING` = validator state → `STAKE` policy version 2.** params =
  `validator_key(32) ‖ bls_pubkey(48)` = 80 B (fits the cap unchanged); commitment = owner
  binding as v1; valid from H+1 like v1. The PoP is creation *authorization*, not state: it
  rides in the v1 carrier (`PUSH(84: "B3S2" ‖ vk ‖ pk) PUSH(96: pop) OP_2DROP <owner script>`)
  and is verified in `CheckStakeOutputs` at creation, outside the policy model (it moves to
  the creation-action area when the codec lands). Binding authority = the staker, as for the
  v1 validator key — no BIP340 binding signature. Finality weight counts only ACTIVE v2
  outputs; if one validator key's v2 outputs disagree on `bls_pubkey`, the largest-weight key
  (tie → lexicographically smaller) is the validator's finality key and the rest do not count.
  Replaces the §2 "`B3B1` output" and the spec's "binding action type 5".
- **`FINALITY_CERTIFICATE` = block-consensus metadata** (not transaction state, not
  validator state). The model-pure home is the coinbase transaction's segregated
  proof/creation-action area — not live and not buildable for the X-pin release. Smallest
  new policy type: **`FINALITY_CERT = 6`** (next free number; never renumbered):
  coinbase-only, ≤ 1 per block, `amount = 0`, native asset, **commitment = `finality_digest`**,
  params = `FinalizedBlock ‖ bitmap ‖ sig` under a type-specific bound (≤ 1,232 B; the
  generic 80-byte cap is unchanged for all other policies), unspendable by definition like
  `BURN`; activated from F. Precedent for block metadata in a coinbase cell: BIP34 height,
  BIP141 witness commitment. When the codec exists the certificate becomes `FINALITY_CERT v2`
  in the proof area; v1 bytes are never reinterpreted. Replaces the §1 "`B3F1` OP_RETURN".
- Reserve creation-action types 4/5 anyway (never reuse); `scriptSig` and `vchBlockSig`
  remain untouched.

## 7.5 Owner corrections of 2026-08-23 (second round) — tree findings and the smallest architecture

Inputs checked: `modern/policy.h` (`ModernOutput{asset, amount, type u16, version u16,
commitment 32 B, params ≤ 80 B}`), `modern/proof.h` (`ModernTransition`/`V2`, proof area
≤ 4,000 B per proof, `ProofAreaCommitmentV2`, "outer transaction codec" explicitly future),
`coins.cpp` (`AddCoin` skips only `IsUnspendable()` scripts), `validation.cpp` (legacy
zero-value marker deliberately kept out of the UTXO set in Connect **and** Disconnect —
precedent for a consensus-defined non-UTXO output), `kernel/chainparams.cpp`
(**B3 main: `SegwitHeight = INT_MAX`, Taproot `NEVER_ACTIVE`** → any witness byte in a
mainnet block is `unexpected-witness`; script flags P2SH|WITNESS|TAPROOT are nevertheless
always on for execution), `ContextualCheckBlock` (the coinbase witness is pinned to one
32-byte reserved value and is **not** covered by the witness merkle root).

**(1) `FINALITY_CERT` cell + proof payload — invariant preserved, no params change.**
`MAX_POLICY_PARAMS_SIZE = 80` stays global and untouched. The cell is bounded typed state:
`FINALITY_CERT = 6`, v1, `amount = 0`, native asset, **commitment = `finality_digest`**,
**params = `epoch u64 ‖ height u64` (16 B)**. The variable certificate bytes
(`FinalizedBlock ‖ bitmap ‖ sig`, ≤ 1,232 B) are a **creation-authorization payload** —
in the model, creation action type 4 (reserved), payload ≤ 4,000 B, bound to the cell by
`commitment == digest(payload)`. The model-pure segregated home (proof/creation-action area)
has no outer carrier today, and the only segregated area Core offers — the witness — is
(a) not activatable without pinning `SegwitHeight` for the modern era (currently INT_MAX on
main) and (b) **uncommitted for the coinbase** (reserved value only), so a coinbase-carried
certificate can never live in a witness. Two realizations, both keeping the cell identical:
- **R1 (recommended, smallest):** the payload rides in the cell's own script carrier as a
  second push after the typed carrier push — the exact v1 pattern every live policy uses
  (`B3S1` realizes STAKE's owner proof in script). txid → merkle → block hash commits it;
  the script is never executed (no 520-byte limit applies); `ModernOutput` sees 16-B params.
  Not temporary: the cell's model is final; if a V2 outer codec later exists the payload
  moves to the creation-action section as `FINALITY_CERT v2`; v1 bytes never reinterpreted.
- **R2 (model-pure, larger):** pin `SegwitHeight = H+1` in the X-pin release, carry the
  certificate in a **non-coinbase** producer transaction's committed witness (taproot annex
  `0x50‖cert`, sighash- and wtxid-committed) with the cell in that transaction. Needs the
  segwit pin, annex semantics, and a per-certificate UTXO spend. Offered, not recommended.

**(2) BLS binding — identity-authorized, never elected by stake.** The "largest stake wins"
rule is withdrawn. New policy **`FINALITY_KEY = 7`**, v1: **commitment = `validator_key`**
(the x-only identity is the cell's binding), **params = `bls_pubkey(48) ‖ seq u32` (52 B ≤ 80)**;
creation payload = **BIP340 signature by `validator_key`** over
`TaggedHash("B3/FINALITY/BIND/V1", ModernChainDomain ‖ validator_key ‖ bls_pubkey ‖ seq)` **and
the BLS PoP** (separate; PoP DST). Consensus: `seq` must equal the validator's previous
binding `seq + 1` (first = 0) — the binding log is an identity-authorized, replay-proof
state-transition sequence; `bls_pubkey = 0^48` = explicit revocation; a `bls_pubkey` may be
active under one `validator_key` at a time; valid from H+1. Derived state
`binding[validator_key] = (bls_pubkey, seq, height)` (connect/disconnect/reindex like the
stake registry). **Snapshot:** for each `validator_key` with ACTIVE STAKE weight `w`
(STAKE **v1 unchanged**, weight aggregation per key unchanged), the member is
`(binding[validator_key].bls_pubkey, w)` iff a non-revoked binding exists at the snapshot
height; stake contributes only through the validator-authorized active binding; rotation =
the next `seq`, effective at the next snapshot boundary. (Variant if `SegwitHeight = H+1`
is pinned anyway: a spendable `FINALITY_KEY` UTXO cell with owner script `OP_1
<validator_key>`, rotation = key-path spend + recreate; equivalent semantics, larger.)

**(3) Consensus-committed, never in the spendable UTXO set — verified, one rule needed.**
Today only `IsUnspendable()` (OP_RETURN-led / > 10,000 B) scripts stay out of the UTXO set;
a typed-carrier script would be added. Since OP_RETURN is rejected as the model, both
metadata cells need an explicit modern rule `IsMetadataCell(script)` → never added in
`AddCoins`, skipped in `DisconnectBlock`'s exact-match check — the precedent is the legacy
zero-value marker (`validation.cpp:2428–2435`). `amount = 0` keeps value accounting
neutral; relay needs a standardness carve-in for `FINALITY_KEY` transactions (STAKE got
one); the coinbase `FINALITY_CERT` never relays.

**Smallest architecture (summary):** two new policy types `FINALITY_CERT = 6` and
`FINALITY_KEY = 7`, both `amount = 0`, both **metadata cells excluded from the UTXO set by
rule**, both with ≤ 80-B typed params and a 32-B commitment, each with a consensus-verified
creation payload realized (v1) as a second carrier push; STAKE v1, `MAX_POLICY_PARAMS_SIZE`,
`ModernOutput`, M3 and the block wire format untouched. **Required ruling:** R1 vs R2 for
the payload position, and whether `SegwitHeight` is to be pinned for the modern era at all
(a separate, pre-existing gap: with INT_MAX no witness-program output is spendable post-H).

## 7.6 Status after the 2026-08-23 rulings

**Constants frozen 2026-08-23** (E 1,440; I/D 10/12; MAX_EPOCH_EXTENSION 7·E; MIN_FINALITY_SET 4 = bootstrap floor only; cost budget 120,000 / 12,000, costs 2,000 / 700, 1 vbyte per unit; MPA 32,768 / 65,536 / ×4). §7.2 items 1–7 are thereby all resolved except the SegWit audit item (separate). Implementation plan: [b3-modern-pos-v1-implementation-plan.md](b3-modern-pos-v1-implementation-plan.md).


F-1 resolved (cells 6/7/8 + MPA Path B, frozen numbers), F-2 adopted in the normative spec as the owner's handover-gated requirement (parameters open), F-3 adopted (F = M in the X-pin release). Weight ×4, cost budget, relay vsize and `ptxid` ruled; byte ceilings open pending benchmark; SegWit activation remains a separate modern-era audit item, not a finality dependency.

## 8. Verdict

With F-1 (carrier), F-2 (gated rotation) and the parameter rows added, Modern PoS V1 is
**born compatible** with BLS finality and Ethereum verification: the signed object is
deterministic and fixed-width, committed by the block hash, produced by a quorum of the
stake-weighted set whose lineage starts at `Snapshot(M−1)`, attested set-by-set, verifiable
on Ethereum from roots alone, and replaceable by a ZK prover without touching any
structure. No bridge mechanism was added or changed.

# Legacy FN issuance — builder / verifier model

Status: **OWNER-LOCKED DIRECTION** (rulings 2026-08-17/18). Recorded in
the master handoff's 2026-08-17/18 reconciliation note and conflict
register **C-R4**; `b3-fn-pod.md` §8 carries the matching supersession
banner. This file is the full design + implementation record. The
superseded designs (funding-key scan-and-claim, production PodDB
wiring) are catalogued in §8.

## 1. The model (owner ruling, 2026-08-17)

Legacy disintegration transactions (PoDs) remain nothing but confirmed
legacy transactions. **No node maintains production PoD state.**

- **One archival builder** (any wallet/node holding complete legacy
  history — an archival or reindexed node holding complete block
  history; no txindex is required) constructs, for each qualifying
  legacy PoD, a canonical **proof-carrying FN issuance transaction**: an
  ordinary modern colored-coin issuance that "looks like someone in the
  modern era just issued FN Coin". It holds these in a private local
  queue before the modern activation height **M** and broadcasts them at
  or after M.
- **Every other node** verifies a received issuance transaction using
  only (a) the transaction and its embedded proof, (b) the fixed H/X
  consensus anchor (plus the node's own block index, which every node —
  pruned or not — permanently holds), and (c) the modern chain's own
  issued-PoDId set accumulated from M onward. No legacy rescan, no
  derived database, no external state, no trusted list.
- The builder has **no authority**: anyone with the historical data
  constructs byte-identical canonical transactions. **Deduplication and
  uniqueness are enforced per PoDId** (the one-issuance-per-PoDId rule;
  the future `issued[pod_id]` state) — never by proof or transaction
  byte identity. The canonical encoding rules (§4) remove every KNOWN
  proof malleability and honest builders emit byte-identical bytes, but
  byte identity is a practical determinism property, not the dedup
  authority.
- No developer/admin key. No funding-key claim signatures. No user claim
  process. Recipient selection is a fixed canonical rule over the
  historical bytes.

## 2. Trust base of verification

1. **H/X anchor** (consensus params): the block at height H must be X;
   no reorganization may ever cross the boundary once X is on the active
   chain (`DisconnectCrossesLegacyBoundary`, `AbandonOffAnchorTip`).
2. **Every node's block index** persists each block's `hashMerkleRoot`
   (`CDiskBlockIndex`, `chain.h:398`) for the whole chain, even when
   block data is pruned. Height → merkle-root lookup on the active chain
   below H is therefore anchored by X and available on every node
   forever. This is consensus state every node already has — not
   external state.
3. **Legacy transaction identity**: a legacy txid commits to the full
   legacy serialization including `nTime`
   (`CTransaction::ComputeHash`, `primitives/transaction.cpp:84` —
   `TX_LEGACY_B3`). Legacy block merkle roots are computed over exactly
   these txids and were mutation-checked at acceptance
   (`CheckBlock` → `BlockMerkleRoot`, `validation.cpp:4555`).

So a merkle path from a legacy transaction to the committed
`hashMerkleRoot` of a block at height h ≤ H on the active chain **is** a
proof of membership in the ledger sealed by H/X.

## 3. Existing primitives (inspected 2026-08-17)

| Piece | Where | Status |
|---|---|---|
| Merkle path computation | `TransactionMerklePath` (`consensus/merkle.h:34`) — legacy-txid-aware via `GetHash()` | exists |
| Merkle path **verification** (fold leaf+path+position → root) | `ComputeMerkleRootFromPath` (`consensus/merkle`) + the canonical fold `CanonicalMerkleRootFromPath` (`modern/legacy_fn_issuance.h`) | **implemented**, tested incl. malleability rejection |
| Legacy tx codec / txid | `TX_LEGACY_B3`, `CTransaction::ComputeHash` | exists |
| Height → merkle root, anchored | block index + H/X boundary rules | exists |
| PoD qualification rule | `legacy::GetFNCollateral` + detector (non-coinbase, non-coinstake, gap ≥ tier) — single source shared with `node::ClassifyPod` | exists |
| FN v1 output binding | `modern/fn.h`: global non-native `FN_ASSET_ID`, whole units 1–1000, ownership-policy commitment, **EMPTY v1 params** (the original per-PoD-params note here was superseded by the corrected 2026-08-18 model — PoDId lives only in issuance evidence/nullifier state) | exists, corrected |
| Proof carrier | `modern/creation_action.h` CreationAction (payload ≤ `MAX_CREATION_ACTION_PAYLOAD` = 4,000 B, pinned equal to `MAX_TRANSITION_PROOF_SIZE`) | exists; **bound is the main tension, §7** |
| Builder-side PoD discovery | `node::DerivePodRecords` / sync scan (`node/fn_pod.{h,cpp}`) | exists; becomes builder-only tooling |
| Funding-tx location for embedding | `node::BuildAllLegacyFnIssuances` deterministic three-pass archival sweep (discovery → targeted funding lookup → build), no txindex dependency | **implemented**; builder-only |

## 4. Proposed proof format — `LegacyFnIssuanceProofV1`

Rides as the payload of a new creation-action type
(`CREATION_ACTION_LEGACY_FN_ISSUANCE`, version 1) in the issuance
transaction. All integers are canonical compact sizes; decoding is
strict (minimal encodings, bounded reads, full consumption, no trailing
bytes). Two further **canonical merkle-position rules** close the known
position malleabilities: a position may carry no bits above its path
length (folding never consumes them), and at a level whose sibling
equals the running hash (the odd-tree self-duplication) the node must
sit on the left — `Hash(h, h)` is order-independent, so the right-side
encoding would be a second verifying byte sequence. With these rules
honest encodings are unique; issuance uniqueness is nonetheless
enforced per PoDId (§1), never by bytes. Embedded evidence transactions
are bounded by the frozen legacy block-size limit (master `main.h`
`MAX_BLOCK_SIZE` = 5,000,000 bytes) — an evidence bound over sealed
history; the future issuance-carrier limit remains OPEN (§7).

```
u8      proof_version          = 1
varint  pod_height             h,      0 < h ≤ H
varint  pod_tx_len ; bytes     pod_tx           (full legacy serialization)
varint  pod_position                            (index of pod_tx in block h)
varint  pod_path_len ; pod_path_len × u256      (merkle path, deepest first)
varint  n_funding                               (== count of DISTINCT prevout txids
                                                 of pod_tx, in order of first
                                                 appearance in pod_tx.vin)
n_funding × {
    varint  height_i           ≤ H
    varint  funding_tx_len ; bytes funding_tx_i (full legacy serialization)
    varint  position_i
    varint  path_len_i ; path_len_i × u256
}
```

What each part proves:

- `pod_tx` + `(pod_height, pod_position, pod_path)` → the PoD
  transaction is in the sealed chain; **PoDId := legacy txid of
  `pod_tx`** — the issuance receipt/nullifier checked against
  `issued[pod_id]`, and nothing else. (This bullet originally bound the
  PoDId into the FN output's params; superseded by the corrected
  2026-08-18 model — FN v1 params are empty and PoDId never enters
  outputs.)
- Each `funding_tx_i` + its path → the funding transaction is in the
  sealed chain; its txid must equal the referenced `prevout.hash`, so
  `vout[prevout.n]` reveals the **exact committed input value and
  funding scriptPubKey** for the gap computation and the recipient rule.
  No undo data is used anywhere — undo is not committed to by the chain
  and is therefore unprovable; full funding transactions are the
  smallest *provable* value evidence.
- Validity of the disintegration itself (inputs existed, were unspent,
  signatures) is **inherited from the sealed chain**: the legacy chain
  already validated `pod_tx` when it confirmed it. The embedded evidence
  only reveals committed bytes; it re-proves nothing.

## 5. Verifier — `LegacyFnIssuanceVerifier`

Inputs: the issuance transaction, the decoded proof, chain params, a
chain view (height→block hash AND height→merkle root, both answered
from the node's own index for the same chain), and the issued-PoDId
predicate (modern consensus state from M onward).

**Step 0 — anchor the view itself:** the verifier requires H/X pinned
in params AND `block_hash_at(H) == X` from the supplied view before
trusting any of its merkle roots. A view bound to any other chain — an
off-anchor tip before recovery has run, a different network — verifies
nothing. (Implemented; wrong-X rejection and exact-H acceptance are
pinned by tests.)

**Precise scope of `VerifyLegacyFnIssuanceAction` as implemented:** it
checks (1) historical eligibility (anchored membership, proven input
values, gap ≥ tier, the 1-coin P2PKH designation), (2) the beneficiary
ownership-commitment binding of the referenced FN output, (3) the
output's full corrected shape — internally derived non-native
`FN_ASSET_ID`, **amount exactly 1** (implemented in this inactive
verifier), FN policy v1, empty params — and (4) the caller-supplied
duplicate predicate. **PoDId is issuance evidence / the nullifier key
only; it is never an FN-output field and the verifier never looks for
one.** What remains future work: production transaction-level
enforcement (the exactly-one-issuance/output rule over whole
transitions), persistent `issued[pod_id]` and supply-counter state, and
conservation wiring — the height-M spec.

1. Strict-decode the proof; require `pod_height ≤ H` and every
   `height_i ≤ H`.
2. Decode `pod_tx` with the legacy codec; reject if coinbase or
   coinstake; reject any evidence transaction whose serialization is
   exactly 64 bytes (leaf/inner-node ambiguity hardening, same rationale
   as post-2017 SPV hardening).
3. Fold `PoDId` up `pod_path` at `pod_position`; require equality with
   the indexed `hashMerkleRoot` at `pod_height`.
4. For each input of `pod_tx`, resolve its funding entry (distinct
   txids, first-appearance order — mis-ordered or missing entries are
   invalid); verify each funding path the same way; require
   `funding_txid == prevout.hash` and `prevout.n` in range; accumulate
   `value_in` and reveal scripts.
5. `gap := value_in − Σ pod_tx.vout` ; require
   `gap ≥ legacy::GetFNCollateral(pod_height)` (tier recomputed, never
   trusted from the proof).
6. Apply the canonical recipient rule (§6); require the referenced FN
   output to be exactly {global non-native `FN_ASSET_ID`, amount 1, FN
   policy v1, **empty v1 params**, ownership commitment = the
   deterministic beneficiary commitment}. (This step originally read
   "params = PoDId, locked to that recipient script" — superseded by
   the corrected 2026-08-18 model: PoDId never enters FN outputs, and
   no legacy script is modern locking syntax.)
7. Require the PoDId not already issued; block-level rule keeps one
   issuance per PoDId.

Everything is deterministic; two honest verifiers cannot disagree, and
two honest builders produce byte-identical transactions.

## 6. Canonical recipient rule — grounded in the historical client

Inspected on the legacy `master` branch (2026-08-17). The old client's
own FN identity binding was:

- `CFNSigner::IsVinAssociatedWithPubkey` (`signhelper_mn.h:20`): the
  registered FN outpoint's **parent transaction** must contain an output
  of **exactly `1*COIN`** whose script is **exactly P2PKH of the
  registration pubkey** (`payee2.SetDestination(pubkey.GetID())`,
  compared by script equality).
- `AcceptableFundamentalTxn` (`main.cpp:825`, called at
  `fn-manager.cpp:685` — "make sure it's burnt trnasaction"): that same
  parent transaction must satisfy
  `valueIn − valueOut ≥ GetFNCollateral(height)`.
- `SelectCoinsFundamentalnode` (`fn-activity.cpp:396`): operators could
  only register outputs of exactly `1 * COIN`.

So historically, **the FN's reward identity was a 1-COIN (1 old-B3 =
1,000,000 units) P2PKH output inside the disintegration transaction
itself.** The owner also confirms operators sometimes *spent* that
1-coin output later — which is irrelevant here: the association reads
the transaction's bytes (`GetTransaction`), not the UTXO set, and the
sealed chain preserves those bytes forever.

**Canonical rule (owner ruling 2026-08-17, faithful to the above):**

- The recipient is the scriptPubKey of the **lowest-index output of
  `pod_tx` with value exactly `1*COIN` and byte-exact P2PKH form** — the
  operator's own historical designation. (Multiple 1-COIN P2PKH outputs
  in one PoD would historically have been disambiguated by the off-chain
  registration; lowest index is the deterministic stand-in — expected to
  be moot on the real chain, verifiable with the §7 measurement pass.)
- **A disintegration with no such output is IGNORED**: no FN issuance
  exists for it, ever, and no fallback recipient is derived. It remains
  exactly what it always was — a confirmed legacy transaction. (It could
  never have registered an FN in the historical client either.) The
  builder skips it; the verifier rejects any issuance claiming it.
- **The historical script is proof evidence only, never modern locking
  syntax.** No modern output carries a legacy P2PKH/P2SH/P2WSH
  scriptPubKey; the beneficiary selects the initial modern owner
  through the deterministic ownership-policy COMMITMENT (SHA256 of the
  canonical historical script — the existing script-hash ownership
  form), and control matters only when the modern FN output is later
  spent under modern ownership authorization.

Eligibility for issuance is therefore: non-coinbase, non-coinstake,
`gap ≥ tier(h)`, **and** at least one 1-COIN P2PKH output.

`b3-fn-pod.md` currently treats marker outputs as audit-only and derives
beneficiary authority from funding scripts + fresh signatures; that
claim model is superseded (§8). Under R1/R2 no fresh signature is ever
checked, so no script-form restriction on *funding* inputs is needed —
the old `claimable / UNSUPPORTED_FUNDING_SCRIPT` split becomes
irrelevant to issuance.

## 7. Size analysis and the 4,000-byte bound

Proof size ≈ `pod_tx` + Σ `funding_tx_i` + 32 B × Σ path depths + ~20 B
framing. A typical 1-input PoD in small legacy blocks: ≈ 0.8–1.2 KB —
comfortably inside the existing 4,000 B creation-action bound. But the
bound is breached by PoDs with many inputs or physically large funding
transactions, and funding-transaction size is a historical fact we
cannot change.

**Recommendation:** before pinning any bound, extend the offline
capacity report (`BuildPodCapacityReport` / `b3coin-utxo-verify
-podreport`) to compute the exact V1 proof size for every real mainnet
PoD. Then either (a) keep 4,000 B if every real PoD fits, or (b) set a
dedicated `MAX_LEGACY_FN_ISSUANCE_PROOF_SIZE` from the measured maximum.
Measurement first; no guessed constant.

## 8. Supersessions and conflicts to reconcile (reported, not resolved)

1. `modern/fn.h` `FnAuthorization`/`FnClaimActionV1` (funding-key
   signatures, committed inactive) — **superseded** by this model: no
   user claim, no fresh signatures. To be retired in a reviewed commit.
2. `b3-fn-pod.md` §8.2/§8.4 production-database and claim-flow language
   — superseded: no production PodDB, no every-node derivation. The PoD
   *detector* and scan machinery survive as builder-side tooling.
3. The uncommitted nine-file PodDB production-wiring diff — obsolete in
   direction; disposition decided by the owner (working tree currently
   holds it plus partial rework; nothing committed or discarded).
4. Open (activation-phase, height M spec): are queued issuances merely
   *valid* from M, or mandatory block content; FN output amount
   semantics; relay policy for proof-carrying transactions.
5. **The canonical FN story (owner wording correction 2026-08-18):**
   FN Coin is one global chain-scoped colored asset with decimals = 0
   and a lifetime issuance cap of 5,000 units (ratified 2026-08-22; R = 3,500 historical PoDs). **HISTORICAL FN:** a
   legacy PoD permanently destroyed native B3 historically; a later
   modern historical issuance verifies that sealed event and authorizes
   exactly one FN Coin for the historical beneficiary. **MODERN FN
   (future work):** a modern PoD will destroy native B3 in the modern
   transaction itself, and its validated issuance authorizes the same
   +1 FN Coin directly in the modern era. The common invariant — valid
   PoD → permanent native-B3 sacrifice → authorized issuance of exactly
   +1 FN_ASSET_ID — holds in both eras; they differ ONLY in where the
   PoD evidence originates and when the destruction occurred. It is NOT
   the case that all destruction is legacy-era or all creation is
   modern-era as a universal rule, and in neither era does FN Coin
   replace, refund, denominate, or recreate the destroyed B3. PoDId is
   only the one-time issuance receipt/nullifier and never travels with
   ordinary FN Coin. Structural amount bounds are NOT mint authority:
   `ParseFnOutput`'s `1..1000` range means representable, never
   creatable — minting is exclusively +1 per validated issuance under
   the conservation equation.

5a. **Modern creation cost curve — PINNED (owner ruling 2026-08-28,
   before the v1 release: "we need solid numbers first and these will
   be fixed forever"):** `RequiredDisintegration(modern_creations_ever)`
   is now implemented in `src/modern/fn.h` as pinned consensus
   constants, closing the "numbers OPEN" item of b3-fn-pod.md §11.1.
   The 5,000 cap splits as `FN_HISTORICAL_RESERVED = 3,500` (reserved
   perpetually, free of modern cost, never advancing the curve) plus
   `MAX_FN_MODERN_CREATED = 1,500` modern slots priced by count:

   | modern slot | cost per FN | tier total |
   |---|---|---|
   | 1..500 | 15,000 B3 | 7.5M B3 |
   | 501..1000 | 30,000 B3 | 15M B3 |
   | 1001..1500 | 60,000 B3 | 30M B3 |
   | 1501+ | no slot, forever | — |

   Anchored so modern FN #1 costs exactly the cheapest historical tier
   (15M old-B3 = 15,000 B3); a full sellout destroys 52.5M B3, bounded
   by the low estimate of historical destruction (3,500 × 15,000..25,000
   = 52.5M..87.5M B3). Nondecreasing over the modern-creations-ever
   counter, integer base-unit arithmetic, exhausted permanently at slot
   1,500 (extinguishment never reopens a slot). Boundary values are
   static_assert-pinned in the header and swept by
   `fn_claim_tests/required_disintegration_curve`. Ships in v1 as a
   public consensus commitment; the modern-PoD *transaction* validation
   that charges this price remains future work gated on FN activation.

6. **FN identity — LOCKED and IMPLEMENTED (owner's corrected
   specification, 2026-08-18):** FN Coin is ONE global, chain-scoped,
   fungible-but-indivisible modern colored asset: `decimals = 0`,
   `MAX_FN_EVER_ISSUED = 5000` (ratified 2026-08-22), every unit under the same
   `FN_ASSET_ID = TaggedHash("B3/FN/ASSET/V1") << ModernChainDomain`
   (derivation pinned on synthetic vectors; the MAINNET id is pinned
   only after mainnet H/X freezes the domain). PoDId is ONLY the
   one-time issuance receipt/nullifier (`issued[pod_id]`, future
   consensus state) — never asset identity, never stored in ordinary FN
   outputs, never copied through transfers. FN v1 outputs are
   `{asset = FN_ASSET_ID, amount = whole units, PolicyType::FN v1,
   ownership-policy commitment, EMPTY params}` — no per-PoD objects, no
   NativeAsset FN, no opaque params, no "same-PoDId-successor" transfer
   lifecycle. The historical beneficiary maps to the modern ownership
   commitment as SHA256 of the canonical 25-byte P2PKH script (the
   existing script-hash ownership form; no pubkey needed at issuance,
   no HASH160→key conversion exists). Issuance rides
   `LegacyFnIssuanceActionV1` (creation-action type 2, v1: proof +
   fn_output_index); the superseded `FnClaimActionV1` keeps its
   reserved type (1, 1) and frozen codec, is UNSUPPORTED for issuance,
   and is rejected by the issuance decoder — old bytes never acquire
   new meaning. Ownership structure and divisibility stay separate
   concepts: a threshold commitment over amount 1 is one jointly
   controlled unit, never fractions. Full colored-coin transferability
   mechanics remain the owner's upcoming design; nothing implements
   transfer enforcement, balances, or any persistent state.

## 9. Explicitly not in scope of the next implementation step

Wallet queue/broadcast automation, the M activation rules, mempool
policy, mining, RPC, and any consensus wiring: the first buildable step
is the pure proof format + builder + verifier as libraries with tests,
activation-inert.

## 10. Implementation (2026-08-18, uncommitted, owner-approved "go")

- `src/consensus/merkle.{h,cpp}`: `ComputeMerkleRootFromPath` — the
  exact fold inverse of `TransactionMerklePath`.
- `src/modern/legacy_fn_issuance.h`: the V1 proof codec (strict,
  injective), `IsLegacyFnP2pkh` / `FindLegacyFnRecipientVout` (the §6
  rule, no fallback), the 64-byte evidence hardening, the stateless
  verifier (`VerifyLegacyFnIssuanceProof` / `VerifyLegacyFnIssuance`)
  and the pure self-verifying builder (`BuildLegacyFnIssuanceProof`).
  **Owner-binding decision taken:** the issued FN output's OWNER
  commitment is `LegacyLockCommitment(recipient_script)` — the same
  SHA256-of-script construction LEGACY_LOCK v1 already uses for
  historical scripts, so the recipient proves control exactly as any
  legacy-locked value.
- `src/node/legacy_fn_issuance.{h,cpp}`: `BuildAllLegacyFnIssuances`,
  the archival three-pass sweep (undo-based discovery + eligibility
  filter, funding location without a txindex, pure-builder assembly).
- `src/test/legacy_fn_issuance_tests.cpp`: fold round-trips, recipient
  rule, strict-codec rejections, offline end-to-end build/verify with a
  full per-field sabotage suite, builder refusals (short gap, no
  designation, 64-byte shape), unpinned refusal, and the chain-backed
  archival sweep with independent stateless re-verification and
  byte-identical determinism.

The §7 measurement pass over real mainnet history (extending
`-podreport` with exact V1 proof sizes) remains the open operator step
before any payload bound is pinned.

Surgical hardening pass (2026-08-18, owner-directed, same uncommitted
diff): the verifier now anchors the chain view itself (`LegacyChainView`
with `block_hash_at` + `merkle_root_at`; block at H must be exactly X
before any root is trusted — wrong-X and missing-hash rejection plus
exact-H acceptance tests); the evidence-transaction bound was corrected
from a wrong 1,000,000 to the frozen legacy 5,000,000 (`master`
`main.h:28` `MAX_BLOCK_SIZE`; the future issuance-carrier limit stays
OPEN); `CanonicalMerkleRootFromPath` closes both known merkle-position
malleabilities (unused high bits; the side bit at odd-tree
self-duplication levels — the tests first DEMONSTRATE each malleability
through the plain fold, then pin its rejection); and every
byte-canonicality claim was demoted to a practical determinism property,
with deduplication explicitly per PoDId.

Corrected FN model pass (2026-08-18, owner's corrected specification,
same uncommitted diff — all inactive, wired into nothing): §8.5's locked
model implemented. `modern/fn.h`: `FnAssetId` (+ pinned synthetic
vectors), `MAX_FN_EVER_ISSUED`, corrected `FnOutputView` /
`MakeFnOutput` / `ParseFnOutput` (global asset, whole units in
[1, 1000] — zero FN balance is represented by NO output, never a
zero-amount output — EMPTY canonical v1 params, enforced, no opaque
bytes),
`FnSupplyModel` / `CheckFnUnitConservation` (pure cap/extinguishment/
conservation model; no persistent counters), funding-key sections
banner-marked SUPERSEDED with frozen bytes. `modern/policy.h`: FN v1
structural rules corrected (non-native asset, empty params).
`modern/creation_action.h`: type 2 `CREATION_ACTION_LEGACY_FN_ISSUANCE`
registered; type 1 reserved/superseded; the generic layer stays generic.
`modern/legacy_fn_issuance.h`: `LegacyFnIssuanceActionV1` codec (rejects
type 1 outright), `LegacyFnMintAuthorization`, and
`VerifyLegacyFnIssuanceAction` implementing §12's eleven checks against
an outputs list; the beneficiary→commitment mapping documented and
vector-pinned. Tests: `fn_claim_tests` (asset-identity vectors, supply
model, corrected output vectors, superseded-record cases) and
`legacy_fn_issuance_tests` (two-PoD same-asset identity, native/wrong
asset rejection, exactly-one-unit, policy shape, index, beneficiary,
duplicate-predicate, superseded-action rejection, no-PoDId-in-output).

Second surgical pass (2026-08-18, owner inspection findings, same
uncommitted diff): live FN amounts corrected to [1, 1000] (zero balance
= no output); `CheckFnCreationActions` now SEMANTICALLY REJECTS every
type-1 action — generic framing still decodes the reserved bytes, no FN
checker accepts them (envelope test proves decode-then-reject); the
issuance verifier derives the FN asset INTERNALLY from
`ModernChainDomain(genesis, X)` and fails closed when the domain is
underivable — no caller can nominate or bless an asset (tested with an
arbitrary non-native asset and a null genesis); the pure supply helpers
enforce `live_supply <= issued_total <= 1000` before acting and the
conservation check is pre-add overflow-guarded (UINT64_MAX wraparound
attempts tested).

**Honest status limits (pre-activation gates, recorded not waived):**

1. **Carrier coverage is UNPROVEN.** The type-2 action rides the
   generic 4,000-byte creation-action payload bound, which is
   inactive/provisional. Whether every REAL historical proof fits is
   unmeasured; **FN activation remains blocked** until the archival
   builder confirms every real proof fits that bound, or a reviewed
   versioned carrier is selected. No claim of complete historical
   coverage is made.
2. **Ownership authorization is NOT implemented.** The
   beneficiary→policy-commitment REPRESENTATION exists (SHA256 of the
   canonical P2PKH script), but actual FN/OWNER signature and threshold
   authorization does not — `VerifyTransitionProof` currently requires
   only a nonempty payload. Real spend authorization (script reveal +
   signature verification over the future modern sighash, and any
   threshold scheme) is a MANDATORY PRE-ACTIVATION item. Shared or
   threshold ownership is a structural commitment format today, not an
   operational feature.

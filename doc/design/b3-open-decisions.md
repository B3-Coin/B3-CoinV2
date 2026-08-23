# B3 Open Decisions

Decisions that are **not locked**. Nothing here may be resolved by implementation choice.
If work appears to require settling one of these, **stop and request the decision**.

Locked material lives in [b3-architecture-contract.md](b3-architecture-contract.md).

---

## OD-1 — Modern PoS consensus specification — **V1 MECHANISM FROZEN (2026-08-20); numerics REVISABLE**

**Status: the V1 mechanism is FROZEN by explicit owner rulings (M1–M6,
2026-08-20) and implementation is authorized.**
[b3-modern-pos-spec.md](b3-modern-pos-spec.md) is the frozen V1 specification:
deterministic stake-weighted hash eligibility over a chained seed, exact
deterministic timestamps encoding recovery rounds, BIP340 validator block
signatures, PoS-native height/round fork choice with a fixed reorganization
horizon, and an unconditional modern coinbase cap. No VRF, epochs, committees,
slashing, finality gadget, or delegation in V1 (all V2 research, spec §10).

What remains OPEN under OD-1 (after the 2026-08-21 ratifications: block
interval 60 s, round length 30 s, f0 = 1, ×2 relaxation, and the STAKE v1
carrier are all RATIFIED; the corridor reward is ratified fees-only and
fail-closed; cutoff C and the readiness gate are ruled out of existence):

- The sentinel-bits and future-drift values stay provisional (spec §9).
- The modern reward schedule (with OD-2).
- The corridor difficulty VALUE (policy ruled: low, stall-safety dominant,
  calibrated to a single CPU core), measured at H/X pinning time.

Further RATIFIED 2026-08-21: the horizon D = 1440 (one day at the 60 s
interval) and `min_stake_amount` = 333 modern B3 (kB3; 333e9 base units),
both stated on mainnet and inert until H/X.

Resolved by the frozen spec (formerly listed as undefined): stake eligibility
(STAKE policy outputs, aggregated per key, 20-block depth); the selection
function (tagged-hash digest over the seed chain — no separate retarget: the
`w/W` normalization is the difficulty); reward cap (unconditional, outside the
validator); PoS block structure (coinbase key declaration + trailing BIP340
signature); block signature scheme (BIP340); validator set (the derived stake
registry; no committees; finality is fork-choice-plus-horizon only in V1).

Constraints already locked and not open for reinterpretation:

- The modern block header stays Bitcoin-style; it is **not** redesigned merely because
  consensus is PoS (contract §17).
- Modern PoS must be complete before FlowMesh (contract §53).
- Validators and FlowMesh FNs are separate roles (contract §52).

**Required to unblock mainnet activation:** the horizon D, the remaining
provisional values (sentinel bits, future drift, modern reward with OD-2),
the corridor difficulty value, `min_stake_amount`, and the real-history
equivalence gate, as always. The timing numbers and the STAKE carrier are
ratified; implementation on regtest is complete.

**The transition model is AUTHORITATIVE design direction (2026-08-16):**
[b3-during-fork-transition.md](b3-during-fork-transition.md) — the
**temporary-PoW corridor**. Both earlier models (post-boundary
self-activating bootstrap; the 1,000-block legacy-PoS declaration window)
are SUPERSEDED. Timeline: Genesis…500 historical B3 PoW
(`LAST_POW_BLOCK = 500`); 501…H legacy PoS with X = hash(H) the immutable
anchor; H+1…H+1000 temporary PoW reusing B3's existing scrypt PoW primitive
in **modern-format blocks** with Policy Outputs active, in which holders
spend legacy UTXOs once (LEGACY_LOCK) into real modern STAKE outputs that
mature undisturbed (legacy coinstake churn is why declarations inside
legacy PoS were abandoned); M = H+1001 first modern-PoS block, starting
from the registry derived deterministically at the end of H+1000. Corridor
length 1,000 is a locked count; per-validator weight aggregation is LOCKED
(no per-UTXO lottery tickets); `STAKE_ACTIVATION_DEPTH = 20` (mature iff `h − b ≥ 20`) is the
preserved design number; after H, legacy PoS never resumes. Operator keys,
snapshots, committees and self-authorizing blocks remain forbidden.

**Mainnet activation stays gated** on the corridor document's remaining
OPEN list — after the 2026-08-21 rulings that list is: the corridor
difficulty VALUE (mechanism ruled: fixed constant, no retarget), corridor
reorg-depth bounds and §7 mitigations, minimum stake amount, and
X-distribution pause-vs-precommit — plus the horizon D and the remaining
spec-§9 provisional values. Ruled out of existence on 2026-08-21: cutoff C
(the 20-block activation depth alone governs), the readiness/minimum-stake
consensus gate (operational, options A+C), and the corridor
reward/capture question (ratified 0, fees-only, fail-closed). The earlier
PD-1..17 register is superseded by the frozen V1 spec; the initial seed at
M is resolved (spec §3).

---

## OD-2 — Modern B3 monetary policy

Native B3 supply continues from the historical ledger (contract §19), but the modern-era
issuance curve, staking reward rate, and fee-burn behaviour (if any) are unspecified.
Overlaps with OD-1's reward schedule. Native B3 must never be issued through the generic
coloured-asset engine.

---

## Colored assets simple-v1 — RULED 2026-08-22 (formerly audit Decision 11)

Owner rulings: (1) asset identity unified with the FN convention —
`AssetId = TaggedHash("B3/ASSET/V1") ‖ ModernChainDomain ‖ issuance_outpoint
‖ H(genesis record)` (chain-bound, rule-bound; supersedes the untagged
outpoint-only derivation and reconciles contract §21 by strengthening it);
(2) the v1 asset-wide rule set is exactly `max_supply`, `decimals` and
`mint_authority = NONE`, stated once in the issuance transaction's creation
action and never repeated on outputs; (3) the genesis mints exactly
`max_supply` in one transaction and no later mint exists, so the cap holds
by construction without a registry. ROOM FOR EXPANSION (owner direction,
same day): the genesis record carries an `issuance_mode` byte (v1 accepts
only GENESIS_FIXED; AUTHORITY_MINT, POW_MINED — PoW-minable colored assets
— and BRIDGE_BACKED are RESERVED numbers) plus a bounded `mode_params`
blob (empty in v1), so a future mode ships its parameters inside the same
record layout and the same AssetId preimage with no format change;
`max_supply` is the hard cap in every mode. Programmable schemes and the
reserved modes themselves stay V2 research. Implemented in
`src/modern/asset.h` (test-only activation unchanged).

## OD-3 — Consensus governance / upgrade mechanism

Contract §43 and §55 require a deterministic activation mechanism and a governed
`AcceptedFeeAssets` registry, but the governance mechanism itself is undefined: who may
propose, what ratifies a change, and how activation heights (A1/A2/A3) are chosen and
encoded.

---

## OD-4 — FN supply economics

Contract §51 explicitly leaves the issuance curve outside consensus. The old
"every 25 FN → price doubles" scheme is **rejected** (cartel/oligopoly failure mode). The
conceptual direction is license scarcity + bond + performance-based revenue + B3 burn for
new entry, but no curve, bond size, or revenue split is locked. The policy interface may
exist; the economics may not be implemented.

**Creation mechanism locked (2026-08-16, [b3-fn-pod.md](b3-fn-pod.md)):** modern
FN creation preserves the historical **Proof of Disintegration** — implicit
destruction through the transaction accounting gap, never claimable as a fee,
never a generic BURN output — modernized with an explicit on-chain FN
ownership (FN Coin) output. FN Coin is a separate asset/state from B3; one
PoD event creates at most one FN. OPEN here: the modern PoD amount, FN Coin
issuance rate/quantity, excess-gap treatment, ownership serialization,
transfer rules and reward economics.

---

## OD-5 — FN claim derivation details

Contract §49 requires a deterministic claim set reproducible from legacy history. The
derivation rule is not specified: exactly which historical facts identify a
FundamentalNode, who the eligible beneficiary is, and what the claim amount is.

**Known blocker:** current accounting only tracks the *aggregate*
(`CBlockIndex::m_legacy_fn_integrated`) and detects FN transactions by a heuristic
(a non-coinstake transaction whose fee ≥ the tiered collateral). It records **no
per-owner identity**, so no beneficiary set can be derived from present data. Extracting
owner identities requires a dedicated pass over legacy history — worth deciding early,
since it may influence what trusted replay captures.

**Recoverability boundary established (2026-08-16, [b3-fn-pod.md](b3-fn-pod.md)):**
the historical operator/pubkey binding lived in P2P broadcasts and is
unrecoverable from chain data. Recoverable: every disintegration transaction,
its gap/height, and its outputs — including the customary 1-B3 marker output.
The natural derivable claim rule is therefore "beneficiary = controller of the
disintegration transaction's marker output"; any rule requiring network-layer
state is impossible.

---

## OD-6 — FlowMesh epoch ↔ B3 finality relationship — **RULED 2026-08-22**

`FLOWMESH_ANCHOR_DEPTH = 30` blocks (~30 min at the 60 s interval) governs
deposit recognition, anchor acceptance, and receipt redeemability alike;
distinct from `MODERN_REORG_HORIZON = 1440`. Certificate finality (yes,
FlowMesh-state only, conflicting certificate = evidence + fail-safe halt),
the keyless receipt-vault (ratified, redeemability = certified ∧ anchor
canonical ∧ buried ≥ depth ∧ not consumed), and the DEX_VAULT v2 beneficiary
binding (USER_DEPOSIT vs VAULT_POOL_CHANGE) are recorded in
[b3-flowmesh-dex-decisions.md](b3-flowmesh-dex-decisions.md) §3b. The text
below is the pre-ruling statement of the question.

## OD-6 (original statement)

Contract §39 requires microblocks/epochs to be anchored to B3 rather than forming an
independent history, but the binding is unspecified: the relationship between a FlowMesh
`slot`/epoch and block height, the anchoring cadence, the finality rule a deposit must
satisfy (contract §30), and what happens to a finalized receipt if its anchoring block is
reorged. Today `flowmesh::Ledger::m_slot` is a bare counter with no defined relationship
to the chain.

---

## OD-7 — Per-market FlowMesh precision

Contract §36 requires each market to define consensus precision (price ticks, quantity
lots, fee units, funding units, margin precision). No values or governing mechanism are
specified.

---

## OD-8 — Bridge design

Contract §21 requires bridged assets to encode origin domain, and §45 requires bridge
security to stay explicit, but no bridge verification mechanism, finality assumption, or
issuer-freeze policy handling is specified. Gated well behind H+1 (activation A3).

**Direction RULED 2026-08-22 (mechanism details still OPEN):**

- No bridge is a dependency of FlowMesh: the first real quote/fee asset is the native CDP-backed bUSD (DEX register L-6); bridged stables are optional extra liquidity added later.
- Any protocol-level bridge is **light-client / SPV on the mint leg**, never a signer set inside consensus: Ethereum via the sync-committee light client (finalized headers only; the owner ruled the light client as "the solution"), Bitcoin via SPV proofs of the most-work chain. The release leg (B3 → origin) needs the origin chain to verify B3 finality (a B3 Modern-PoS light client there) and until then runs through a rotatable signer set or an optimistic scheme — the one bridged-asset policy carries a rotatable `signer_set` so v1-managed → outsourced → light-client → issuer-native are in-place transitions of the same `AssetId` (`bridge_instance` pinned; signer set is mutable state of the instance).
- Tron and other non-light-client chains are served **off-consensus** (Chainflip-class swaps into native B3, managed on-ramps); no Tron bridge enters consensus.
- End state: an issuer taking mint authority in place (native issuance) — a business outcome, not a protocol dependency.
- Recommended, not yet ruled: Ethereum **L1** (not an L2) as the stable origin — an Arbitrum origin adds a rollup-state proof and ~6.4-day BoLD finality or sequencer trust, and its USDT is USDT0 (LayerZero-backed).
- Still OPEN: exact light-client verification rules and the `blst`/keccak/RLP/MPT dependency decision, sync-committee participation threshold, mint caps, watcher veto, fork-upgrade procedure, re-bootstrap rule, issuer-freeze handling.
- **2026-08-23 owner direction: "BLS is the key to bridge."** BLS12-381 aggregate signatures are the keystone of BOTH legs — verifying Ethereum's sync committee (mint leg) and, with BLS keys on B3's committee, letting Ethereum verify B3 checkpoints via EIP-2537 (release leg) — so no trusted signer set remains in either consensus. Plan, inventory (blst vendoring, Keccak-256, SSZ/RLP/MPT, `BRIDGE_BACKED` + `LIGHT_CLIENT_UPDATE` + `BRIDGE_MINT`/`BRIDGE_BURN` actions, B3 BLS committee), staged build order and the remaining owner decisions: [b3-bridge-bls-proposal.md](b3-bridge-bls-proposal.md). Still not authorized for consensus wiring before v1 ships and proposal §8 is ruled.
- **2026-08-23 owner brief: the critical problem is B3 → Ethereum (withdrawals), not deposits.** Design record: [b3-finality-to-ethereum.md](b3-finality-to-ethereum.md) — a V2 finality gadget layered on Modern PoS V1 (BLS key binding, one-epoch-lookahead validator-set snapshot committed by keccak header + Merkle members, `FinalizedBlock{height, block_hash, withdrawal_root, validator_set_hash(successor), epoch}` + `Certificate{signer_bitmap, aggregate_sig}` verified on B3 in-block with an absolute finality pin, `B3FinalityVerifier.sol` with set handover + non-signer-subtraction BLS verification via EIP-2537, cumulative withdrawal tree, vault release only on finality ∧ inclusion, `IB3FinalityProver` seam for a future ZK prover with frozen data structures). **Superseded the same day by the owner's correction:** the finality gadget and BLS validator keys are **V1-reserved from M** (PoS spec ruling M7), enforcement at F, bridge at A3 ≥ F; the verifier follows genesis set → epoch certificates → rotation → withdrawal roots. Revision 2 of the same document is the specification (exact gadget, key lifecycle, quorum, epoch transitions, permanent Ethereum state, attack table). Still design only ("do not implement yet"); owner decisions in its §9.
- **2026-08-23 direction ACCEPTED by owner; protocol FINALIZED:** [b3-cross-chain-finality-v1.md](b3-cross-chain-finality-v1.md) (normative) — BLS finality is part of Modern PoS V1; BIP340 identity key and BLS consensus key strictly separate; Ethereum verifier built on the fixed-depth (13) keccak `members_root` + 126-byte set header, never a member list; exact verification of epoch transition (strict e→e+1 after successor disclosure), weights (absentee leaves + multiproof), bitmap (LSB-first, complement-of-absentees check), quorum (`floor(2W/3)+1`, rule enforced on-chain for ruleset 1), withdrawal root (depth-32 cumulative keccak tree, single tree with `origin_chain_id` in the leaf). Remaining: parameters in its §9 (F, E, intervals, set bounds, lag) and the implementation go-ahead.
- **2026-08-23 owner: ZK deferred** — BLS certificate prover only for v1; `IB3FinalityProver` seam retained, no ZK work scheduled.
- **2026-08-23 compatibility audit:** [b3-finality-compatibility-report.md](b3-finality-compatibility-report.md). Three findings need rulings before code: F-1 the creation-action section has no live carrier in the tree → script-level carriers (`B3F1` coinbase OP_RETURN certificate, `B3B1` binding output) recommended; F-2 validator-set rotation must be handover-gated (epoch extends until `Set_e` certifies), not fixed-height; F-3 the v1 binary never reaches M (X blank, PoS params unset) so **F = M lives in the X-pin release** without `blst` in v1. The OD-8 interim 'rotatable signer_set' release-leg fallback is superseded by the finality protocol (strike by ruling).
- **2026-08-23 owner LOCK (80-byte model):** `policy_params ≤ 80 B` is a permanent invariant for small typed live/derived state; large evidence (BLS certificates, bridge/Merkle/ZK proofs) is bounded, priced payload data committed to by the policy object — never policy state, never an arbitrary-data policy, never solved by raising `MAX_POLICY_PARAMS_SIZE`. `FINALITY_CERT` = small typed metadata (commitment = hash of the full payload, strict type max, ≤ 1 where required); `FINALITY_KEY` = small binding state (BIP340 identity authorizes the BLS key + separate PoP, sequence-controlled rotation/revocation, effective at next snapshot). Native carrier: [b3-modern-payload-area.md](b3-modern-payload-area.md) rev. 2 — MPA **accepted** (BIP144 flag 0x02 + the existing strict creation-action section), commitment **Path B ruled**: coinbase-only zero-value `MODERN_PAYLOAD_ROOT` metadata cell (policy 8, never in UTXO) committing `payload_root = ComputeMerkleRoot(TaggedHash("B3/MPA/LEAF/V1", i ‖ section_hash_i))`; non-circularity proven (leaves carry position + section hash, never a txid); `ptxid` = full-serialization hash for relay (no SegWit dependency; SegWit activation stays a separate audit question); resource numbers NOT frozen — worst case shows a per-block verification-cost budget is required (PoP spam ≈ 18 s/block at 4 MB) and the MPA weight factor must be chosen first.
- **2026-08-23 rulings applied to the normative documents:** MPA weight ×4; consensus payload verification-cost budget (per-tx and per-block, checked before cryptography, deterministic per `(type, version)`); relay vsize includes verification cost; `ptxid` defined normatively over the canonical full serialization; policy numbers 6/7/8 frozen (contract §23 updated); byte ceilings NOT frozen — framework frozen, numbers after benchmark. Finality spec rev. 2 now carries the cells + MPA records, identity-authorized `FINALITY_KEY`, handover-gated rotation, F = M in the X-pin release. Remaining owner decisions before Modern PoS implementation: see the session report of 2026-08-23 (finality spec §9, MPA §9, PoS spec §9 provisional rows, SegWit audit).
- **2026-08-23 Tier-1 rulings + benchmark:** gated rotation FINAL; epoch window `{current, current−1}` FINAL under monotone-height / set-hash / epoch-relation conditions; BLS binding mandatory for block eligibility from F = M (one stake universe); `FINALITY_KEY` semantics FINAL. Benchmark-only work authorized and done: pinned `blst` v0.3.17 vendored (`src/blst`, build-off-by-default), harness `b3-finality-bench`; results + recommended constants in [b3-finality-benchmark-2026-08-23.md](b3-finality-benchmark-2026-08-23.md) (PoP verify ≈ 0.6 ms; certificate ≈ 1.1 ms @3,500 / 1.9 ms @8,192; recommend I/D = 10/12, E = 1,440, cost budget 120,000 / 12,000 units, 1 vbyte/unit, ceilings 32,768 / 65,536). Consensus implementation still awaits a separate go-ahead.
- **2026-08-23 CONSTANTS FROZEN (owner):** E = 1,440; CHECKPOINT_INTERVAL = 10; CHECKPOINT_DEPTH = 12; MAX_EPOCH_EXTENSION = 7·E; MIN_FINALITY_SET = 4 (chain bootstrap floor only — bridge security thresholds are an A3 decision); verify_cost FINALITY_KEY_EVIDENCE 700 / FINALITY_CERTIFICATE 2,000; MAX_BLOCK_PAYLOAD_COST 120,000; MAX_TX_PAYLOAD_COST 12,000; COST_TO_VBYTES 1; MPA record 32,768 B / section 65,536 B / weight ×4. All earlier Modern PoS / finality / MPA rulings remain in force. Implementation plan: [b3-modern-pos-v1-implementation-plan.md](b3-modern-pos-v1-implementation-plan.md) — **implementation awaits explicit plan approval.**

---

## OD-9 — Encrypted order flow

Contract §41 defers threshold encryption pending an MEV/front-running justification. Open
whether it is ever adopted; it must not become a dependency of H+1.

---

## OD-10 — Operational H/X coordination

Contract §62 states the preferred model (choose an already-buried block, record X, ship the
release) and requires H/X be known before the enforcing binary activates. The concrete
activation mechanism — how nodes agree to begin modern validation once H/X are baked in —
is not finalized and must not be improvised late.

**RULED 2026-08-23 (owner):** H = 820,000 (corridor 820,001..821,000,
M = 821,001); X distribution = PAUSE, FAIL CLOSED — a release ships with H
set and X blank, accepts through H and refuses every block at H+1
(`legacy-boundary-unpinned`, a no-penalty header refusal, plus a
production guard), and a mandatory follow-up release pins X and resumes
the corridor; nodes with blank X never enter the corridor. Corridor bits =
canonical `0x1f008000`. Seeds: `176.31.13.198` plus at least two further
independently hosted fixed seeds and an owner-controlled DNS seed before
final release. **None of these values is pinned in mainnet chainparams
until the four pin gates in [b3-release-v1.md](b3-release-v1.md) pass.**

---

## Ratified deviations (closed — recorded so they are not "fixed" later)

- **Policy enum numbering.** `LEGACY_LOCK = 0`, `OWNER = 1`, `BURN = 2`, `DEX_VAULT = 3`
  are consensus-stable as serialized. `BURN` occupying slot 2 ahead of `STAKE`/`BRIDGE` is
  **accepted as-is**; existing numbers must never be renumbered (contract §23). New policy
  types take new numbers.
- **`policy_params` field.** `ModernOutput` carries a sixth field (`policy_params`, ≤ 80
  bytes) beyond the five-field conceptual model, used by `DEX_VAULT` v1 for a shard id. It
  is already in the frozen wire vector and is retained.
- **No tagged modern block hash.** Modern block hashing stays the existing modern domain;
  domain tags apply only to newly introduced hashing, not block or existing transaction
  identity (contract §7).
- **No transaction-family byte.** Block-context codec selection plus trusted per-connection
  context for standalone transactions is the locked mechanism (contract §8).
- **Sync policy.** "Outbound-only sync ownership" is **not** locked. The requirement is
  bounded, progress-safe sync (contract §59); any design meeting it is acceptable.

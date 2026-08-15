# B3 Open Decisions

Decisions that are **not locked**. Nothing here may be resolved by implementation choice.
If work appears to require settling one of these, **stop and request the decision**.

Locked material lives in [b3-architecture-contract.md](b3-architecture-contract.md).

---

## OD-1 — Modern PoS consensus specification — **UNRESOLVED (blocking)**

**Status: UNRESOLVED at the protocol-detail level. Do not implement.**

Modern PoS is the single unbuilt piece on the critical path to H+1, and its protocol
details have not been specified. `src/modern/pos.h` deliberately **fails closed**:
`modern::CheckModernStake` rejects every modern-era block with `no-modern-pos-rules`
unless a test-only validator is installed. That is the correct state until a spec exists.

Undefined and required before any implementation:

- **Stake eligibility** — which UTXOs may stake; minimum amount; minimum/maximum age;
  whether policy-typed outputs (e.g. a `STAKE` policy) are required or optional.
- **Kernel / selection function** — the hash construction, its inputs, and its randomness
  source. Legacy stake modifiers and legacy in-block transaction offsets are explicitly
  **not** carried forward.
- **Difficulty / target retarget** — algorithm and bounds for the modern era.
- **Reward schedule** — modern B3 monetary policy, including the coinbase/subsidy cap.
  (Note: today the modern branch has no issuance cap other than the fail-closed hook —
  see IS-3 in the status document.)
- **Modern PoS block structure** — how a modern block declares itself proof-of-stake.
- **Block signature / attestation scheme.**
- **Validator set and finality** — membership, rotation, and whether finality is
  committee-certified.

Constraints already locked and not open for reinterpretation:

- The modern block header stays Bitcoin-style; it is **not** redesigned merely because
  consensus is PoS (contract §17).
- Modern PoS must be complete before FlowMesh (contract §53).
- Validators and FlowMesh FNs are separate roles (contract §52).

**Required to unblock:** a written modern PoS consensus specification covering every bullet
above.

**Design base accepted:** [b3-modern-pos-spec.md](b3-modern-pos-spec.md) is the
accepted structural base (STAKE policy outputs, locked-amount weight,
owner/validator key split, no auto-compounding, bounded age, VRF eligibility
with ranked fallbacks, cheap pre-verification). Every remaining decision is
tracked there with LOCKED/OPEN status: PD-1..PD-17 are all OPEN and all numeric
parameters are simulation-gated.

**The transition model is AUTHORITATIVE design direction (2026-08-15):**
[b3-during-fork-transition.md](b3-during-fork-transition.md) — the fork is a
staged process, not an instantaneous migration. The final 1,000 legacy
blocks `T … F` (W = TRANSITION_LENGTH = 1,000) are the transition window:
ordinary legacy blocks under unchanged legacy PoS, inside which upgraded
wallets create legacy-compatible stake declarations; the era/format/PoS
switch happens exactly once, at `F → M` (M = F+1), and the initial modern
validator set derives deterministically at (F, X) from qualifying unspent
declarations (cutoff C splits initial ACTIVE from PENDING). The earlier
post-boundary self-activating bootstrap analysis is superseded; operator
keys, trusted validator lists and passive-balance snapshots are forbidden.
The existing code's boundary (`hard_fork_height` = M; `LegacyFinalHeight` =
F; `legacy_final_hash` = X) is unchanged; T/W/C are future additive
constants.

**OD-1 stays UNRESOLVED and nothing may be implemented until every remaining
OPEN item is explicitly locked** — the PoS PDs (PD-1..15, 17) plus the
transition document's OPEN list (declaration encoding and versioning,
validator-key type, minimum stake, cutoff depth F−C, readiness thresholds,
duplicate resolution, declaration indexing, initial seed at M, exit
authorization flow, X distribution pause-vs-precommit, relay policy); the
numeric ones lock only from simulation results.

---

## OD-2 — Modern B3 monetary policy

Native B3 supply continues from the historical ledger (contract §19), but the modern-era
issuance curve, staking reward rate, and fee-burn behaviour (if any) are unspecified.
Overlaps with OD-1's reward schedule. Native B3 must never be issued through the generic
coloured-asset engine.

---

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

---

## OD-6 — FlowMesh epoch ↔ B3 finality relationship

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

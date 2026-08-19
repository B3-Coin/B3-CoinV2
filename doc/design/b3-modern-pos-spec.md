# B3 Modern PoS — design specification

**Status: ACCEPTED AS DESIGN BASE — no implementation.** The structure below is
the agreed base for modern PoS. Every remaining decision carries an explicit
status: **LOCKED** (binding) or **OPEN** (must be explicitly locked before any
implementation; never resolved by implementation choice). All numeric protocol
parameters are OPEN pending simulation — no number in this document is a
consensus assumption. OD-1 in [b3-open-decisions.md](b3-open-decisions.md)
stays unresolved until every OPEN item is locked; until then
`modern::CheckModernStake` keeps failing closed (`no-modern-pos-rules`), and
that behavior is pinned by `legacy_transition_tests/`
`non_empty_transition_fails_closed_at_h_plus_one`.

## 1. Inherited constraints — LOCKED (by the architecture contract)

- Modern block header stays Bitcoin-style (80 bytes, SHA256d identity); no
  redesign for PoS (contract §17); no new hash domain for block identity.
- Modern fork choice selects only among descendants of X; reorganizations
  across H are prohibited; H+1 is intentionally minimal.
- Value lives in typed Policy Outputs; policy enum values are append-only
  (`LEGACY_LOCK=0, OWNER=1, BURN=2, DEX_VAULT=3`).
- `ModernChainDomain = H("B3/MODERN/CHAIN" || genesis || X)` is the
  anti-replay domain for genuinely new signed structures — never block
  identity.
- Validators and FlowMesh FNs are separate roles. Native B3 is never issued
  through the asset engine. Modern issuance carries an explicit cap.

## 2. Agreed direction — LOCKED (by the user)

1. **STAKE is a Policy Output.** Consensus stake exists only as an explicitly
   typed output; no plain UTXO stakes implicitly.
2. **Independent per-wallet STAKE outputs**; each is an independent consensus
   object; no registration authority. (Outputs are independent as *value
   objects*; eligibility nevertheless aggregates per validator key — §5 —
   so output-splitting confers no advantage.)
3. **Locked B3 is the consensus weight** — the amount in an *active* STAKE
   output, nothing else.
4. **Owner key ≠ validator key.** The owner commitment controls the funds;
   a distinct validator key evaluates eligibility and signs blocks; a
   compromised validator key cannot spend the stake.
5. **Rewards never auto-increase weight.** Rewards pay to ordinary outputs;
   consensus rejects a reward paying into any STAKE output; more weight
   requires an explicit new STAKE output with its own activation delay.
6. **Stake age does not grow forever.** After activation, weight is constant;
   nothing resembles legacy coin-age.
7. **VRF-based eligibility** — publicly verifiable, privately evaluable,
   non-grindable.
8. **Multiple eligible proposers per slot with ordered fallbacks**, so an
   offline primary cannot stall a slot.
9. **Cheap pre-verification**: eligibility verifiable from block bytes plus
   the parent's derived registry before any transaction processing.

## 3. Data model — DESIGN BASE (structure locked; constants OPEN)

### 3.1 The STAKE policy output (v1)

New appended policy type `STAKE = 4`, `policy_version = 1`:

- `asset` — native B3 only.
- `amount` — the locked amount; the weight once active.
- `policy_commitment` — 32-byte owner binding (OWNER-v1 commitment scheme).
- `policy_params` — 32-byte x-only validator public key (key type: PD-2) plus
  a 2-byte reserved field, within `MAX_POLICY_PARAMS_SIZE`.

Lifecycle (fully derivable from the chain):

    created (in block b)
      → PENDING   until activation delay elapses      (no weight)
      → ACTIVE    weight = amount                     (eligible)
      → owner spends into an explicit UNSTAKE intent
      → COOLDOWN  until unlock delay elapses          (no weight, unspendable)
      → spendable as ordinary value

The activation delay makes epoch seeds non-grindable by just-in-time stake
creation; the unlock cooldown keeps misbehavior attributable before funds
exit. Both delays are OPEN constants (PD-7).

### 3.2 The derived stake registry

Per connected block, consensus maintains the **active set** — every ACTIVE
STAKE outpoint with (weight, validator key, activation height) — and total
active weight `W`. Derived state: recomputable from the UTXO set, maintained
incrementally at connect/disconnect. It is the input to cheap eligibility
verification. In-block commitment of the registry: PD-15 (OPEN).

## 4. Slots, epochs, randomness — DESIGN BASE (mechanism locked; numbers OPEN)

Time divides into slots of `SLOT_SECONDS` (OPEN, PD-3); a block's `nTime`
must lie inside its claimed slot, slots strictly increasing along a chain,
empty slots skipped. Slots group into epochs of `EPOCH_SLOTS` (OPEN, PD-4).
The active set and `W` used by an epoch are frozen at the epoch boundary, so
mid-epoch weight changes cannot re-roll current eligibility. Each epoch has a
seed whose derivation (PD-4) must deny any proposer a cheap grinding window
and must be fixed no later than the stake snapshot it applies to.

## 5. Eligibility and anti-stall — DESIGN BASE (mechanism locked; numbers OPEN)

For slot `s` in epoch `e`, a validator with active weight `w` computes

    y, π = VRF_sk(ModernChainDomain || seed_e || s)

and is eligible iff `y < T(w, W)`, where `w` is the validator's **aggregated**
weight — `SUM(all qualifying ACTIVE STAKE principal assigned to this
validator key)` — never per-output. **This aggregation rule is LOCKED**
(2026-08-16): splitting 100,000 B3 across 10,000 STAKE outputs must confer
exactly the proposer opportunity of one output; there is one VRF evaluation
per validator identity per slot, not one lottery ticket per UTXO. `T` is
calibrated (PD-5) so the expected eligible count per slot is a small
constant `K`. Eligible validators rank
by ascending `y`; rank 0 is the primary, higher ranks are fallbacks; rank `r`
may not timestamp a block before `slot_start + r · RANK_DELAY` (OPEN, PD-5).
Cheap verification uses only the header, the proposer proof and the parent's
registry snapshot: membership, VRF proof, threshold, rank/time ladder, block
signature — no transaction execution.

## 6. Block structure — DESIGN BASE (placement question OPEN as PD-6)

The header is untouched, so the PoS declaration lives in the body, split to
resolve the sign-your-own-hash circularity exactly as the legacy codec did:

- **Proposer proof** — inside the merkle-committed data at a fixed early
  position (PD-6): staked outpoint, slot index, claimed rank, VRF proof `π`.
- **Block signature** — a trailing modern-codec field outside the committed
  data: validator-key signature over `ModernChainDomain || block_hash`.
  Block identity (header SHA256d) is unchanged.

Modern `nBits`/`nNonce` semantics: PD-9 (OPEN).

## 7. Fork choice — DESIGN BASE (mapping OPEN as PD-10)

Selection stays "most chain weight" over the existing `nChainWork`
accumulator among descendants of X. A modern block's weight is a monotone
function of proposer rank (better rank → more weight), giving primary
preference, deterministic tie-breaks (ascending `y`), and no withholding
advantage. The exact mapping and its bounds are OPEN (PD-10); a modern
reorg-depth bar is OPEN (PD-14).

## 8. Rewards — DESIGN BASE (economics OPEN)

The reward transaction pays to ordinary (OWNER) outputs; consensus rejects a
reward directed into any STAKE output (locks direction item 5 at the
consensus level). Amounts, decay, fee treatment and maturity are OPEN
(PD-11/PD-12, jointly with OD-2), under the locked issuance-cap invariant.

## 9. Misbehavior — DESIGN BASE (posture OPEN as PD-13)

Equivocation is at minimum handled by fork choice, with the unlock cooldown
as the attribution window; whether evidence-based stake burning (via the
existing BURN policy) ships at H+1 or behind a later activation height is
OPEN (PD-13).

## 10. Initial validator set — RESOLVED BY THE TEMPORARY-PoW CORRIDOR

**Superseded twice and now settled in direction** (2026-08-16, authoritative
user direction): both the post-boundary "self-activating bootstrap" and the
1,000-block *legacy-PoS declaration window* are SUPERSEDED. The transition
is a temporary-PoW corridor with modern semantics:

    Genesis…500        historical B3 PoW        (LAST_POW_BLOCK = 500)
    501…H              legacy B3 PoS            X = hash(H), the anchor
    H+1 … H+1000       TEMPORARY B3 PoW corridor — modern block/tx format,
                       Policy Outputs active, real STAKE outputs created
                       from legacy UTXOs via LEGACY_LOCK and matured
    M = H+1001 onward  modern B3 PoS

There is no bootstrap circularity and no declaration indirection: during
the corridor, holders create **actual modern STAKE Policy Outputs**; block
production is temporary PoW (reusing B3's historically proven scrypt PoW
primitive), so legacy coinstake churn never conflicts with stake creation.
At the end of H+1000 every node derives the same initial validator registry
from qualifying mature STAKE outputs (cutoff height C, OPEN); at M the
modern eligibility rule selects the first proposer from that registry. The
authoritative corridor specification is
[b3-during-fork-transition.md](b3-during-fork-transition.md). The initial
randomness/VRF seed at M remains OPEN. Every "H+1 = first modern-PoS
block" statement elsewhere in this document predates the corridor and
reads as **M = H+1001**; H+1 is the first modern-*format* (temporary-PoW)
block.

## 11. Pending decisions — ALL OPEN

Numeric values that previously appeared as recommendations are **withdrawn as
consensus assumptions**; where retained below they are explicitly
*simulation candidates* — starting points for the simulation phase, carrying
no normative weight. Implementation begins only after every PD is explicitly
locked by the user.

| PD | Question | Options (⊘ = rejected with rationale) | Status |
|---|---|---|---|
| PD-1 | VRF primitive | (a) ECVRF-SECP256K1-SHA256-TAI (RFC 9381), in-tree over vendored libsecp256k1; (b) BLS VRF — new curve dependency; (c) ⊘ deterministic-signature-as-VRF: verifier cannot prove nonce determinism → grindable | OPEN |
| PD-2 | Validator key type | (a) BIP340 x-only Schnorr (32 B, in-tree, same key signs blocks); (b) compressed ECDSA (33 B) | OPEN |
| PD-3 | Slot duration | Simulation question. Candidates 32/64/128 s; constraint: `K_max · RANK_DELAY < SLOT_SECONDS` | OPEN — simulation |
| PD-4 | Epoch length; seed derivation | Length: simulation question. Seed: (a) fold VRF outputs of an early fraction of the prior epoch with a cutoff (denies end-of-epoch grinding); (b) ⊘ last-block hash: last proposer grinds by withholding; (c) pure chaining from prior seed: ungrindable but far-future-predictable (targeted DoS on future proposers) | OPEN — mechanism + simulation |
| PD-5 | Threshold function; K; rank ladder | Function over the validator's **aggregated** weight `w` (per-validator evaluation is LOCKED — one VRF attempt per validator key per slot, never per STAKE output): (a) binomial `P(eligible) = 1−(1−K/W_slots)^w`; (b) linear `y < K·w·2^256/W` (equivalent in the small-p regime). K and RANK_DELAY: simulation questions | OPEN — function + simulation |
| PD-6 | Proposer-proof placement | (a) payload in the reward transaction's first output (front of block, streams early); (b) dedicated proposer transaction at index 1; (c) block-level section before the tx vector (deepest codec change) | OPEN |
| PD-7 | Lifecycle constants | `MIN_STAKE_AMOUNT` (economics input; registry-size bound), `N_activate`, `N_unlock` (relationship to epoch length and attribution window) | OPEN — economics + simulation |
| PD-8 | Validator-key re-delegation | (a) static (change = unlock + re-lock, full delays); (b) owner-signed in-place re-delegation (no weight gap; more consensus surface) | OPEN |
| PD-9 | Modern `nBits`/`nNonce` semantics | (a) fixed sentinels, enforced exactly (threshold is registry-derived; no retarget field needed); (b) `nBits` mirrors epoch calibration for observability (adds a consistency obligation) | OPEN |
| PD-10 | Fork-choice weight mapping | (a) `weight = BASE · (K − rank)` in `nChainWork` units (bounded rank bonus); (b) constant weight, rank as tie-break only (weaker anti-withholding); (c) ⊘ continuous `1/y`: unbounded outliers | OPEN |
| PD-11 | Reward schedule (with OD-2) | (a) flat per-block with step-downs under the cap; (b) epoch yield proportional to locked stake, under the cap; (c) fee-only after a short subsidy | OPEN — economics |
| PD-12 | Fees; reward maturity | Fees: (a) to proposer; (b) burned. Maturity: constant TBD | OPEN — economics + simulation |
| PD-13 | Equivocation posture at H+1 | (a) fork choice only, penalties behind a later activation height (H+1 minimalism); (b) evidence transactions burning locked stake from launch (evidence rules must ship in v1) | OPEN |
| PD-14 | Modern reorg depth bar | (a) rolling depth bound, no-penalty refusal (legacy analog); (b) none (long-range exposure); (c) per-epoch hard finality (adds a finality gadget H+1 does not need) | OPEN — mechanism + value by simulation |
| PD-15 | Registry commitment | (a) derived-only (H+1 minimal); (b) registry root committed at epoch boundaries (light clients; extra obligation) | OPEN |
| PD-16 | Initial validator set at M | **RESOLVED IN DESIGN DIRECTION — the temporary-PoW corridor** ([b3-during-fork-transition.md](b3-during-fork-transition.md)): 1,000 modern-format PoW blocks H+1…H+1000 in which real STAKE Policy Outputs are created from legacy UTXOs and matured; deterministic registry derivation at the end of H+1000; cutoff C splits initial ACTIVE from PENDING; M = H+1001 is the first modern-PoS block. The earlier self-activating, snapshot, operator-key and legacy-declaration-window options are all superseded. Remaining OPEN sub-items live in the corridor document (§12 OPEN list: corridor difficulty and reward, cutoff C, miner-capture rule, insufficient-stake handling, initial seed at M, X distribution, …) | DIRECTION LOCKED; sub-items OPEN |
| PD-17 | Timestamp rules | Future-drift bound (slots), MTP retention, exact slot/nTime binding (the binding itself is part of the eligibility mechanism and not optional; the bound value is a simulation question) | OPEN — simulation |

### Simulation phase

Before locking the numeric PDs (PD-3, PD-4 length, PD-5 K/ladder, PD-7
delays, PD-10 BASE, PD-12 maturity, PD-14 value, PD-17 bound, and the
transition window's numeric OPEN items — cutoff depth F−C and the readiness
thresholds), a simulation must characterize, at minimum: slot-fill rate and
fork rate vs. K and RANK_DELAY under realistic latency; stall probability
vs. offline fraction; seed-grinding advantage vs. the PD-4 cutoff fraction;
initial-set liveness and capture share at M vs. declaration-participation
assumptions; and reorg-depth distributions vs. the PD-14 bar. Simulation
harness design is out of scope for this document and must not touch
consensus code.

## 12. Sequencing

1. Lock the transition window's OPEN items
   ([b3-during-fork-transition.md](b3-during-fork-transition.md) §15) and
   the non-numeric mechanism PDs.
2. Run the simulation phase; lock the numeric PDs from its results.
3. Only then: implementation as a `modern::PosValidator` behind the existing
   dispatch, replacing the fail-closed gate in reviewable steps (data model,
   registry, eligibility verification, connect-time validation, fork choice,
   production). Until every PD is locked, `no-modern-pos-rules` remains the
   modern era's correct, tested behavior and no placeholder logic ships.

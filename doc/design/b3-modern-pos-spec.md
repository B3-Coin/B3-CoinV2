# B3 Modern PoS — design specification (DRAFT)

**Status: DRAFT — PENDING APPROVAL. Not locked. Not for implementation.**
OD-1 in [b3-open-decisions.md](b3-open-decisions.md) remains UNRESOLVED until this
document is approved; `modern::CheckModernStake` keeps failing closed
(`no-modern-pos-rules`) until an approved revision of this specification is
implemented. Nothing below overrides the architecture contract; where a detail
is not yet decided it appears in the **pending decisions** list (PD-*) with
concrete options, and implementation must not pick an option silently.

## 1. Inherited locked constraints (not revisited here)

- The modern block header stays Bitcoin-style (80 bytes, SHA256d identity);
  it is not redesigned for PoS (contract §17), and no new hash domain is
  introduced for block identity.
- Modern fork choice selects only among descendants of X; reorganizations
  across H are prohibited; H+1 is intentionally minimal — anything not needed
  for a clean H+1 is gated behind later activation heights.
- Value lives in typed Policy Outputs (`modern::ModernOutput`); policy enum
  values are consensus-stable and only appended (`LEGACY_LOCK=0, OWNER=1,
  BURN=2, DEX_VAULT=3`).
- `ModernChainDomain = H("B3/MODERN/CHAIN" || genesis || X)` is the anti-replay
  domain for genuinely new signed/hashed structures (never block identity).
- Validators and FlowMesh FNs are separate roles. Native B3 is never issued
  through the asset engine.

## 2. Agreed direction (fixed by the user; the frame of this spec)

1. **STAKE is a Policy Output.** Consensus stake exists only as an explicitly
   typed output; no plain UTXO ever stakes implicitly.
2. **Independent per-wallet STAKE outputs.** Any wallet creates any number of
   STAKE outputs; each is an independent consensus object. There is no
   account-level aggregation and no registration authority.
3. **Locked B3 is the consensus weight.** Weight = the amount locked in an
   *active* STAKE output. Nothing else contributes.
4. **Owner key ≠ validator key.** The owner commitment controls the funds
   (unlock/spend after cooldown); a distinct validator key signs blocks and
   evaluates eligibility. Compromise of the hot validator key must not spend
   the stake.
5. **Rewards never auto-increase weight.** Rewards pay to ordinary (OWNER)
   outputs. Increasing weight requires an explicit new STAKE output, which
   starts its own activation delay.
6. **Stake age does not grow forever.** After the activation delay, weight is
   constant. There is no age multiplier and nothing resembling legacy coin-age.
7. **VRF-based eligibility.** Slot eligibility is decided by a verifiable
   random function evaluated by the validator key — publicly verifiable,
   privately evaluable, non-grindable.
8. **Multiple eligible proposers per slot with ordered fallbacks**, so an
   offline primary does not stall the chain.
9. **Cheap pre-verification.** A block's eligibility claim is verifiable from
   the block bytes plus the parent's derived stake registry — no transaction
   execution, no UTXO/script work — before full block processing.

## 3. Data model

### 3.1 The STAKE policy output (v1)

A new appended policy type: `STAKE = 4`, `policy_version = 1`.

- `asset` — must be native B3 (AssetRef 0).
- `amount` — the locked amount; the consensus weight once active.
- `policy_commitment` — the 32-byte **owner binding** (same commitment scheme
  as OWNER v1). Only the owner authorizes unlock.
- `policy_params` — the **validator binding**: a 32-byte x-only validator
  public key, plus a 2-byte reserved field (fits `MAX_POLICY_PARAMS_SIZE`).
  The validator key evaluates the VRF and signs proposed blocks. (Key type is
  PD-2; re-delegation is PD-8.)

Lifecycle (per output, entirely derivable from the chain):

    created (in block b)
      → PENDING   until b + N_activate          (no weight)
      → ACTIVE    weight = amount               (eligible)
      → owner spends it into an UNSTAKE intent  (explicit tx)
      → COOLDOWN  until spend + N_unlock        (no weight, not yet spendable)
      → spendable as ordinary value

`N_activate` and `N_unlock` are PD-7 constants. The activation delay is what
makes the epoch seed non-grindable by just-in-time stake creation; the unlock
cooldown is what makes equivocation attributable before funds exit.

### 3.2 The derived stake registry

Consensus maintains, per connected block, the **active set**: every ACTIVE
STAKE outpoint with (weight, validator key, activation height), plus the total
active weight `W`. It is derived state (recomputable from the UTXO set and
heights), maintained incrementally at connect/disconnect, and is the input to
cheap eligibility verification. Whether the registry is additionally
*committed* in each block is PD-15.

## 4. Slots, epochs and randomness

Time is divided into **slots** of `SLOT_SECONDS` (PD-3). A block's header
`nTime` must lie in its claimed slot; the slot index is
`(nTime - modern_era_anchor_time) / SLOT_SECONDS`, strictly increasing along a
chain. Not every slot produces a block.

Slots group into **epochs** of `EPOCH_SLOTS` (PD-4). Each epoch has a **seed**;
the VRF binds (seed, slot). The active set and total weight used for an epoch's
eligibility are frozen at the epoch boundary (the state as of the last block of
the previous epoch), so weight changes mid-epoch cannot re-roll current
eligibility. Seed derivation is PD-4; the requirement is that no proposer can
cheaply grind the next epoch's seed and that the seed is fixed before the
stake snapshot it applies to.

## 5. Eligibility, proposer ranking, anti-stall

For slot `s` in epoch `e`, a validator with active weight `w` evaluates:

    y, π = VRF_sk(ModernChainDomain || seed_e || s)

and is **eligible** iff `y < T(w, W)` where `T` is the threshold function
(PD-5) calibrated so the expected number of eligible validators per slot is a
small constant `K` (weight-proportional selection).

Eligible validators are **ranked by ascending `y`**. Rank 0 is the primary
proposer; ranks 1..K-1 are fallbacks. Anti-stall ladder: rank `r` may not
timestamp a block earlier than `slot_start + r · RANK_DELAY` (PD-5), so an
online fallback publishes shortly after a silent primary, and a fully silent
slot is simply skipped (the next slot's eligibility is independent). Fork
choice prefers better-ranked blocks (see §7), so a late primary does not
displace an already-extended fallback chain, bounded by the ladder.

**Cheap verification** of a received block, using only the header, the
proposer proof (§6) and the parent's derived registry: the claimed STAKE
outpoint is ACTIVE in the epoch snapshot with the claimed validator key and
weight; the VRF proof verifies against that key and (seed, slot); `y` meets
the threshold for that weight; the rank ladder permits the claimed rank at the
claimed `nTime`; the block signature verifies. No transaction beyond the
proposer proof is touched, and nothing executes scripts.

## 6. Block structure: proposer proof and block signature

The header is untouched, so the PoS declaration lives in the body, split in
two per the circularity constraint (a signature over the block hash cannot
itself be inside the merkle-committed data):

- **Proposer proof** — *inside* the committed data, at a fixed early position
  (PD-6): the staked outpoint, the slot index, the claimed rank, and the VRF
  proof `π` (with `y` recomputable from `π`). Being merkle-committed, it is
  covered by the header the validator signs.
- **Block signature** — *outside* the committed data, as a trailing
  modern-codec block field (the structural analog of the legacy
  `vchBlockSig`): a signature by the validator key over
  `ModernChainDomain || block_hash`. Block *identity* (header SHA256d) is
  unchanged; like legacy, the trailing signature is not part of the hash.

Header field semantics for modern PoS blocks (`nBits`, `nNonce`) are PD-9.

## 7. Fork choice

Within descendants of X, chain selection remains "most chain weight" over the
existing `nChainWork` accumulator, with modern per-block weight defined by
proposer quality rather than hashing: a block's weight is a monotone function
of its rank (better rank → more weight; exact mapping PD-10). This gives:
primary-over-fallback preference, deterministic tie-breaking (ascending `y`),
and no advantage from withholding (a rank-0 block released late confronts a
chain already longer by more than its rank bonus). Reorg depth in the modern
era is additionally bounded by a rolling bar (PD-14), the modern analog of the
legacy 500-block rule.

## 8. Rewards

- The block reward pays in the block's reward transaction to ordinary
  (OWNER-policy) outputs — **never** into a STAKE output, and connect-time
  validation rejects a reward paying into any STAKE output (this enforces
  "rewards do not automatically increase weight" at consensus level, not as a
  wallet convention).
- Reward amount per block and fee treatment are PD-11/PD-12 (overlapping
  OD-2), under the contract's issuance-cap invariant: the modern branch must
  enforce an explicit cap; supply continues from the attested legacy total.
- Rewards mature for `N_reward_maturity` blocks (PD-12).

## 9. Misbehavior

At H+1 the design is deliberately minimal (PD-13): equivocation (two signed
blocks by one validator key for the same slot) is handled by fork choice, and
the unlock cooldown guarantees an attributable window; an evidence-based
penalty (burning locked stake via the existing BURN policy) is specified as a
later, activation-gated addition unless approval says otherwise.

## 10. Bootstrap at H+1

Stake can only exist in modern blocks, but the first modern blocks need
proposers — the one place the design needs a special rule (PD-16 decides):
the recommended shape is a **transition window** `H+1 .. H+B` during which a
block may be **self-activating**: it carries (as its first non-reward
transaction) a transaction creating a STAKE output from pre-H value, and the
proposer proof may reference *that in-block output* with `N_activate` waived;
eligibility still runs the VRF against the window's deterministic seed
(derived from `ModernChainDomain`, fixed at X). After the window, only
normally-activated stake is eligible. This keeps H+1 permissionless (any
pre-H holder can contend), deterministic, and free of trusted keys.

---

## 11. Pending decisions (PD) — options and recommendations

Approval of this specification means choosing one option for every PD below
(or supplying a better one). **None may be resolved by implementation choice.**

**PD-1 — VRF primitive.**
(a) ECVRF-SECP256K1-SHA256-TAI (RFC 9381) implemented in-tree over the
vendored libsecp256k1 — standard, proof ~80B, verify ~2 EC mults; new
cryptographic code that needs careful review + test vectors. **(Recommended.)**
(b) BLS-based VRF — smaller aggregation story later, but a new curve
dependency (violates the no-new-deps posture).
(c) "Deterministic signature as VRF" (BIP340 over the slot seed) — **rejected
up front**: a verifier cannot prove the signer used deterministic nonces, so
outputs are grindable; listed only to record why it is unacceptable.

**PD-2 — Validator key type.** (a) BIP340 x-only Schnorr keys (32B, in-tree,
same key signs blocks) — **recommended**; (b) compressed ECDSA (33B, breaks
the 80-byte params budget less cleanly, no benefit).

**PD-3 — Slot duration.** (a) 64 s (close to legacy cadence, comfortable
rank ladder) — **recommended**; (b) 32 s (faster, tighter ladder); (c) 128 s
(conservative). Must divide the rank ladder: `K · RANK_DELAY < SLOT_SECONDS`.

**PD-4 — Epoch length and seed derivation.** Epoch length: (a) 2,048 slots
**(recommended)**; (b) 512; (c) 8,192. Seed: (a) fold the VRF outputs of all
blocks in the *first three quarters* of epoch `e-1` into
`seed_e = H(domain || seed_{e-1} || fold)` — the cutoff denies the last
proposers of an epoch a grinding window **(recommended)**; (b) hash of the
last block of `e-1` (simple; last proposer can grind by withholding);
(c) seed chained purely from `seed_{e-1}` (ungrindable but eventually
predictable far ahead, easing targeted DoS on future proposers).

**PD-5 — Threshold function, K, rank ladder.** Threshold: (a) per-output
binomial `y/2^256 < 1 − (1−K/W_slots)^w` i.e. probability ≈ `K·w/W`, safe
under stake-splitting (splitting weight neither helps nor hurts expected
eligibility) — **recommended**; (b) linear cap `y < K·w·2^256/W` (equivalent
in the small-probability regime, simpler arithmetic). Committee target:
K = 6 / 8 / 12 (**8 recommended**). Rank ladder delay: 8 s at 64-s slots
(**recommended**), giving 7 usable ranks.

**PD-6 — Proposer-proof placement.** (a) A dedicated payload in the reward
transaction's first output (front of block, merkle-committed, streams with
the first kilobyte) — **recommended**; (b) a standalone "proposer
transaction" at index 1; (c) a block-level section before the tx vector (a
deeper codec change). All keep the header frozen.

**PD-7 — Stake lifecycle constants.** `MIN_STAKE_AMOUNT` (needs an economics
input — proposal: high enough that the registry stays ≤ ~10⁵ outputs),
`N_activate` (options 720 / 1,440 / one full epoch — **one epoch
recommended**, aligning activation with snapshot boundaries), `N_unlock`
(options 2× / 4× N_activate — **4× recommended**, the attribution window).

**PD-8 — Validator-key re-delegation.** (a) Static: changing the validator
key = unlock + re-lock (simplest, full delay applies) — **recommended for
H+1**; (b) an owner-signed re-delegation transaction updating params in place
(no weight interruption; more consensus surface), possible later addition.

**PD-9 — Modern header field semantics.** (a) `nBits` = fixed sentinel,
`nNonce` = 0, both enforced exactly (threshold comes from the registry, so no
retarget field is needed) — **recommended**; (b) `nBits` encodes the epoch's
`K/W` calibration for external observability (redundant, adds a
consistency-check obligation).

**PD-10 — Fork-choice weight mapping.** With `K` ranks: (a) block weight
`= BASE · (K − rank)` folded into `nChainWork` units (strict primary
preference, bounded rank bonus) — **recommended**; (b) constant weight with
rank only as tie-break (weaker anti-withholding); (c) continuous `1/y`
weight (unbounded outliers; rejected).

**PD-11 — Reward schedule (with OD-2).** (a) Flat per-block reward with
periodic step-down and an absolute issuance cap; (b) percentage-of-locked-
stake yield per epoch (self-adjusting participation incentive) under the same
cap; (c) fee-only after a short subsidy period. Needs an economics decision;
the cap itself is not optional.

**PD-12 — Fee treatment and reward maturity.** Fees: (a) to the proposer
with the reward — **recommended**; (b) burned (BURN policy). Maturity:
(a) one epoch — **recommended**; (b) legacy-like fixed 30.

**PD-13 — Equivocation handling at H+1.** (a) Fork choice only; penalties
deferred behind an activation height — **recommended (H+1 minimalism)**;
(b) evidence transactions burning the equivocator's locked amount from
launch (stronger, but consensus-verifiable evidence rules must ship in v1).

**PD-14 — Modern reorg depth bar.** (a) Rolling depth bound (e.g. 500
blocks, the legacy analog), no-penalty refusal of deeper forks —
**recommended**; (b) none (pure weight; long-range exposure); (c) hard
finality checkpoint every epoch (stronger; adds a finality gadget H+1 does
not need).

**PD-15 — Registry commitment.** (a) Derived-only registry (no in-block
commitment) — **recommended for H+1**; (b) commit the registry root in the
proposer proof each epoch boundary (light-client friendly; extra consensus
obligation).

**PD-16 — Bootstrap window.** (a) Self-activating window `H+1..H+B` as in
§10, `B` = one epoch — **recommended**; (b) pre-H snapshot-derived initial
validator set (requires defining a legacy-era derivation — heavier, touches
attested history semantics); (c) time-boxed operator bootstrap keys
(centralized; politically costly; listed for completeness).

**PD-17 — Timestamp rules.** Future drift bound (a) ≤ 2 slots —
**recommended**; MTP monotonicity retained as-is; slot claimed must match
`nTime` exactly (integral to eligibility, not optional).

---

## 12. What approval unlocks (and what it does not)

Approval of every PD turns this document into the locked modern PoS
specification: OD-1 closes, and implementation proceeds as a
`modern::PosValidator` behind the existing dispatch — the fail-closed gate is
replaced only by the approved rule set, wired in reviewable steps (data
model, registry, eligibility verification, connect-time validation, fork
choice, production). Until then, `no-modern-pos-rules` remains the correct
and tested behavior of the modern era, and no placeholder logic ships.

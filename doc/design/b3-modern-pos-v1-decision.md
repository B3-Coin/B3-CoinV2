# Modern PoS v1 — decision brief

**Status: DECISION REQUEST. Nothing here is chosen. No code authorized.**

[b3-master-handoff.md](b3-master-handoff.md) §2.5 makes Modern PoS v1 the current
priority (build-order item 2). This document exists so the owner can lock it.

It lists every decision that currently blocks implementation, with options, trade-offs
and a **non-binding recommendation**. A recommendation is an argument, not a choice —
handoff §11 rule 6 forbids me from settling any of these, and I have not.

---

## 1. What already exists (verified in this checkout)

| Piece | Where | State |
|---|---|---|
| STAKE Policy Output, `STAKE = 4` | [src/modern/policy.h:85](src/modern/policy.h:85) | implemented |
| STAKE wire form, `B3S1` magic, 32-byte validator key, 2 reserved | [src/modern/stake.h:66](src/modern/stake.h:66) | implemented, **serialization already in use** |
| `STAKE_ACTIVATION_DEPTH` (`h − b ≥ 20`) | [src/modern/stake.h:73](src/modern/stake.h:73) | implemented |
| `ValidatorRecord`, `StakeRegistry`, per-validator aggregation | [src/node/stake_registry.h:51](src/node/stake_registry.h:51) | implemented, derived-only |
| `DeriveStakeRegistry` from the UTXO set | [src/node/stake_registry.h:88](src/node/stake_registry.h:88) | implemented, reorg-safe by recomputation |
| Modern PoS gate | `modern::CheckModernStake` | **fails closed** (`no-modern-pos-rules`), pinned by `legacy_transition_tests/non_empty_transition_fails_closed_at_h_plus_one` |

So the *inputs* to PoS v1 exist. What is missing is the eligibility rule itself, the
block-level declaration, and the liveness machinery — i.e. every PD below.

---

## 2. The schedule question that dominates everything else

**This is the decision I would put first.**

[b3-modern-pos-spec.md](b3-modern-pos-spec.md) §12 gates *every numeric parameter*
behind a simulation phase (slot-fill and fork rate vs. `K` and `RANK_DELAY` under
realistic latency; stall probability vs. offline fraction; seed-grinding advantage;
reorg-depth distributions). That is a substantial body of work, and it stands between
today and any PoS code.

Master handoff §2.5 pushes the opposite way: v1 should be "deliberately small" and the
corridor should not be polished indefinitely.

These can be reconciled, because of a fact in master §2.2: **mainnet `H`/`X` stay unset
until the real-history equivalence gate passes.** No numeric choice made now can reach
mainnet before that gate. So:

| Option | Meaning | Cost |
|---|---|---|
| **A** — full simulation first | Lock every numeric PD only from simulation results, as spec §12 requires | Correct, but PoS v1 does not start for a long time; item 2 stalls |
| **B** — provisional regtest/testnet numbers, simulation before mainnet | Lock the *mechanisms* now; lock numerics as explicitly `REVISABLE-BEFORE-MAINNET` constants used on regtest/testnet only; run the simulation as the gate for mainnet H/X, not for writing code | Builds the machinery now; risk is that a number quietly hardens into an assumption |
| **C** — simulate only the two numbers that shape the mechanism | `K` and `RANK_DELAY` decide whether the rank ladder works at all; everything else can be provisional | Middle path; smaller simulation scope |

**Recommendation: B, with C's discipline.** Every provisional constant declared in one
place, named with a `REVISABLE` marker, and a test that fails if a provisional constant
is referenced under `ChainType::MAIN`. That makes option B structurally safe rather than
merely intended. Under B, "locked mechanism, provisional number" is an honest state and
the fail-closed gate can finally be replaced.

**This is the owner's call. I have not adopted B.**

---

## 3. New decision raised by the master handoff

### PD-18 — Liveness: relaxation, rank ladder, or both

Master §2.5 mandates "variable PoS eligibility target/difficulty" and "failed rounds
relax eligibility." The accepted spec instead over-provisions eligibility per slot
(`K > 1`) and uses a rank ladder as its anti-stall mechanism. See
[b3-master-handoff-conflicts.md](b3-master-handoff-conflicts.md) C-2.

| Option | Mechanism | Trade-off |
|---|---|---|
| (a) Rank ladder only | `K` eligible per slot, ranked by ascending `y`, rank `r` waits `r · RANK_DELAY` | Simplest; no target state to carry. But if *all* `K` are offline the slot is empty, and a long correlated outage stalls until weight returns. Does not satisfy master §2.5 as written |
| (b) Relaxation only | Single eligible proposer; on a failed round the target widens by a defined step | Satisfies §2.5. But it introduces target state into fork choice and creates a new grinding surface: whoever controls perceived round failure influences the target |
| (c) Both | Ladder handles the common case; relaxation is the slow floor for correlated outage | Matches §2.5's "recovery does not instantly reset to an impossible target". Largest consensus surface of the three |

**Recommendation: (c), specified so that relaxation is strictly slower than the ladder** —
the ladder absorbs ordinary offline proposers within a slot, relaxation only engages
after `N` consecutive empty slots and widens by a bounded multiplicative step with a
hard floor, and recovery tightens gradually rather than snapping back. That is exactly
the behavior §2.5 describes; the ladder is what keeps relaxation from being reached in
normal operation.

If (b) or (c) is chosen, **PD-9 changes**: `nBits` can no longer be a fixed sentinel,
because the current target becomes consensus state that a validating node must
reconstruct. PD-5 and PD-10 are also touched. These four move together.

---

## 4. Mechanism decisions — lockable now, no simulation needed

| PD | Question | Recommendation | Why |
|---|---|---|---|
| **PD-1** | VRF primitive | **(a) ECVRF-SECP256K1-SHA256-TAI (RFC 9381)** over the vendored libsecp256k1 | No new curve, no new dependency (the environment forbids installing any), RFC-specified with test vectors. BLS only pays for itself with signature aggregation, and v1 has no committees to aggregate (§2.5 excludes them). Option (c) is already rejected in-spec and correctly so |
| **PD-2** | Validator key type | **(a) BIP340 x-only Schnorr (32 B)** | Effectively constrained already: `STAKE_VALIDATOR_KEY_SIZE{32}` is in shipped STAKE serialization ([stake.h:67](src/modern/stake.h:67)). A 33-byte ECDSA key is a wire change to a form corridor outputs already use. See conflicts C-6 — this should be locked deliberately, not by default |
| **PD-4** (seed mechanism) | Epoch seed derivation | **(a) fold VRF outputs of an early fraction of the prior epoch, with a cutoff** | Denies end-of-epoch grinding (which kills (b)) while staying unpredictable far ahead (which is (c)'s weakness — predictable future proposers are a targeted-DoS list). Only the cutoff *fraction* is numeric |
| **PD-6** | Proposer-proof placement | **(a) payload in the reward transaction's first output** | Smallest codec change and it streams early, so cheap pre-verification (spec §2.9) works on a partially received block. (c) is the deepest change to a codec that is otherwise stable. (b) is defensible if a clean standalone structure is preferred over minimal diff |
| **PD-8** | Validator-key re-delegation | **(a) static for v1** — change = unlock + re-lock with full delays | Master §2.5: v1 adds only what liveness requires. In-place re-delegation is an owner-signed consensus operation with its own replay and timing surface, and nothing in v1 needs it. Deferrable behind a later activation height without a wire change |
| **PD-9** | Modern `nBits`/`nNonce` | **Follows PD-18.** If PD-18 = (a): fixed sentinels, enforced exactly. If PD-18 = (b)/(c): `nBits` must carry the live target and be checked | Do not lock PD-9 before PD-18 |
| **PD-10** | Fork-choice weight mapping | **(a) `weight = BASE · (K − rank)`, bounded** | Gives primary preference and removes the withholding incentive with a bounded bonus. (b) makes rank a tie-break only, which weakens anti-withholding. (c) is already rejected for unbounded outliers. If PD-18 = (b)/(c), the mapping must also be monotone in the relaxation level, or a relaxed chain can outweigh a healthy one |
| **PD-13** | Equivocation posture at M | **(a) fork choice only; penalties behind a later activation height** | Master §2.5 explicitly excludes automatic slashing from v1. Evidence rules are a consensus surface that must ship complete or not at all; the unlock cooldown (PD-7 `N_unlock`) preserves attributability meanwhile |
| **PD-14** (mechanism) | Modern reorg-depth bar | **(a) rolling depth bound with no-penalty refusal** | (c) per-epoch hard finality is excluded by master §2.5 ("no BFT finality in v1"). (b) none leaves long-range exposure. (a) is the legacy analog and is already understood in this codebase. Only the depth *value* is numeric |
| **PD-15** | Registry commitment | **(a) derived-only** | Matches what the code already does ([stake_registry.h:57](src/node/stake_registry.h:57)) and matches v1 minimalism. Committing a registry root is a light-client feature with an ongoing consistency obligation; add it at an activation height when light clients exist |

---

## 5. Numeric decisions — gated on §2's answer

| PD | Parameter | Note |
|---|---|---|
| PD-3 | `SLOT_SECONDS` | Hard constraint regardless of value: `K_max · RANK_DELAY < SLOT_SECONDS`. Candidates 32/64/128 s |
| PD-4 (length) | `EPOCH_SLOTS`, seed cutoff fraction | Cutoff fraction trades grinding resistance against seed freshness |
| PD-5 | Threshold function, `K`, `RANK_DELAY` | The *function* is a mechanism decision that can be locked now: **(a) binomial `P = 1−(1−K/W_slots)^w`** is recommended over the linear approximation — they agree in the small-`p` regime, and the binomial stays correct when one validator holds a large weight share, which is exactly the early-network condition at M. `K` and `RANK_DELAY` are the two numbers most worth simulating |
| PD-7 | `MIN_STAKE_AMOUNT`, `N_activate`, `N_unlock` | **`N_activate = 20` is now LOCKED** by master §2.4 (see conflicts C-4). `N_unlock` and `MIN_STAKE_AMOUNT` remain open; `MIN_STAKE_AMOUNT` is an economics decision that also bounds registry size |
| PD-10 | `BASE` | Follows the PD-10 mechanism choice |
| PD-11/12 | Reward schedule, fee treatment, maturity | Economics, jointly with OD-2. **Not required for v1 liveness** — a block can be produced and validated before the reward curve is final, provided the issuance cap holds. Worth explicitly deferring so it does not block item 2 |
| PD-14 | Depth value | Follows the mechanism choice |
| PD-17 | Timestamp drift bound, MTP retention | The slot/`nTime` binding itself is part of the eligibility mechanism and is **not optional**; only the bound value is numeric |

---

## 6. Corridor items that block PoS v1

From [b3-during-fork-transition.md](b3-during-fork-transition.md) §12. These are
listed as corridor decisions, but M consumes them directly, so item 2 cannot finish
without them (see conflicts C-5):

1. **Cutoff `C`** — splits initial ACTIVE from PENDING at the end of H+1000. Defines the
   exact registry the first PoS block is evaluated against.
2. **Initial randomness/VRF seed at M** — there is no prior epoch to fold from, so the
   PD-4 recursion has no base case. This needs its own rule.
3. **Insufficient-stake handling (options A–D)** — what M does if the derived registry is
   too small or too concentrated to be safe. Master §2.2 forbids silently extending the
   corridor, which rules out the "just keep mining" answer.
4. **Minimum stake amount** — same parameter as PD-7's `MIN_STAKE_AMOUNT`; it should be
   locked once, in one place.
5. **Duplicate/invalid validator-key handling in STAKE outputs** — `StakeRegistry`
   currently aggregates by key with no validity check on the key itself.

---

## 7. Smallest set that must be locked before any code

If the owner wants item 2 to start, this is the minimum:

1. §2 — the schedule posture (A, B, or C).
2. **PD-18** — the liveness mechanism.
3. **PD-1, PD-2** — VRF primitive and key type (they determine the proof format).
4. **PD-6** — where the proposer proof lives (it determines the codec change).
5. **PD-9** — follows PD-18.
6. Corridor **cutoff `C`** and the **initial seed at M**.

With those six, the data model and eligibility verification can be written and tested
on regtest against provisional numbers. PD-10/13/14/15 are needed before fork choice
and production; PD-11/12 are not needed for v1 at all.

---

## 8. What I will not do

- Choose any PD, including the ones where I have argued a clear preference.
- Write a placeholder eligibility function, a "temporary" threshold, or a test-only
  validator that ships enabled.
- Replace the `no-modern-pos-rules` fail-closed gate before the six items in §7 are
  locked. Until then that gate is the correct behavior and its test should stay.

# Master handoff vs. existing design documents — conflict register

**Status: register of conflicts and their resolutions.** Originally report-only;
owner resolutions recorded 2026-08-16 are marked RESOLVED inline. Older text
remains as the record of what conflicted.

[b3-master-handoff.md](b3-master-handoff.md) was placed in the repository on
2026-08-16 as the **top authority** among `doc/design/` documents. The older
documents were deliberately left unchanged — they remain accurate records of what
was decided when. This file catalogues every point where they now disagree, so the
disagreement is visible rather than silently resolved by whichever file a session
happens to read first.

Precedence (master handoff §0): owner's later explicit corrections > proven frozen
legacy behavior > latest approved specs > the master handoff > older brainstorms.

---

## C-1 — Modern PoD encoding: the master handoff **reopens** a repo-locked decision

**Severity: high. Blocks build-order item 5.**

| Source | Position |
|---|---|
| master handoff §4.6 | **OPEN ENCODING.** "There is an unresolved contradiction… Claude must not choose between these while implementing. The project owner must lock the modern PoD encoding." |
| [b3-fn-pod.md](b3-fn-pod.md) §"Decision status" | **LOCKED / DESIGN DIRECTION:** "modern FN creation preserves PoD rather than generic BURN" |
| [b3-open-decisions.md](b3-open-decisions.md) OD-4 | **"Creation mechanism locked (2026-08-16…)":** "implicit destruction through the transaction accounting gap, never claimable as a fee, never a generic BURN output" |

The repo treats *implicit accounting gap* as settled. The master handoff explicitly
lists it as the unresolved choice between the implicit gap and an explicit visible
BURN-style primitive, and forbids me from choosing.

Both documents carry the same date (2026-08-16), so §0's precedence rule does not
separate them by time. Under "master handoff is top authority", the decision is
**reopened**.

**RESOLVED (owner, 2026-08-16):** the modern PoD encoding stays **OPEN** per the
master handoff §4.6 — it is unrelated to legacy claims and must not be selected
during the legacy FN claim work. `b3-fn-pod.md` §2 is marked OPEN accordingly;
OD-4's "creation mechanism locked" wording is limited to the ECONOMIC lineage
(B3 destroyed → FN right; PoD semantically distinct from generic BURN), never
the encoding. Original request kept below for the record.

**Needed from the owner (original):** either (a) reaffirm the `b3-fn-pod.md` lock — implicit
gap — and I annotate master §4.6 as resolved; or (b) confirm it is genuinely
reopened, and OD-4 / `b3-fn-pod.md` are marked accordingly. Until then no modern-FN
creation code can be written either way.

Note the substantive tension the master handoff is pointing at: modern conservation
(§3.3, "no silent asset loss") is in direct friction with an *implicit* gap, because
an implicit gap is by construction invisible to a per-asset conservation check. That
is a real design problem, not a documentation slip.

---

## C-2 — Modern PoS liveness: two different anti-stall mechanisms

**Severity: high. Blocks build-order item 2 (the current priority).**

| Source | Anti-stall mechanism |
|---|---|
| master handoff §2.5 | **Variable eligibility target/difficulty.** "failed rounds relax eligibility"; "recovery does not instantly reset to an impossible target" |
| [b3-modern-pos-spec.md](b3-modern-pos-spec.md) §2.8, §5 | **Over-provisioned eligibility + ranked fallbacks.** Threshold `T(w,W)` calibrated so expected eligible count per slot is a small constant `K`; eligible validators rank by ascending `y`; rank `r` may not timestamp before `slot_start + r · RANK_DELAY` |

These are not the same design. The accepted spec has **no relaxation mechanism** —
its threshold is calibrated per epoch from registry weight, and PD-9 explicitly
contemplates `nBits` as a *fixed sentinel* because "the threshold is registry-derived;
no retarget field needed." The master handoff mandates a target that moves in
response to failed rounds.

They are not necessarily mutually exclusive — a design could carry both — but the
combination has not been specified, and adding relaxation changes PD-5 (threshold
function), PD-9 (`nBits` semantics), and PD-10 (fork-choice weight) together.

**Needed from the owner:** an explicit decision, tracked as **PD-18** in
[b3-modern-pos-v1-decision.md](b3-modern-pos-v1-decision.md).

---

## C-3 — Terminology: "round" vs "slot"/"epoch"

**Severity: low, but must be settled before the spec is written.**

Master §2.5 says "one validator identity gets one deterministic eligibility
evaluation per **round**" and "failed **rounds** relax eligibility."
[b3-modern-pos-spec.md](b3-modern-pos-spec.md) §4 defines **slots** (`SLOT_SECONDS`)
grouped into **epochs** (`EPOCH_SLOTS`), with one VRF evaluation per validator per
slot.

If "round" = "slot", the two documents agree on the evaluation rule and only C-2
remains. If a "round" is a *retry within a slot*, that is a third structure not
present in either document.

---

## C-4 — `STAKE_ACTIVATION_DEPTH = 20`: the master handoff **closes part of** PD-7

**Severity: informational — this is a resolution, not a contradiction.**

Master §2.4 lists `STAKE_ACTIVATION_DEPTH = 20` (`current_height − creation_height ≥ 20`)
as **LOCKED**. [b3-modern-pos-spec.md](b3-modern-pos-spec.md) PD-7 lists the
lifecycle constants `N_activate` and `N_unlock` as OPEN and simulation-gated;
[b3-open-decisions.md](b3-open-decisions.md) hedges it as "the preserved design number."

The master handoff resolves `N_activate = 20`. It says nothing about the **unlock
cooldown `N_unlock`**, which therefore stays OPEN. PD-7 should be recorded as
partially locked.

The code already implements the locked half: `modern::STAKE_ACTIVATION_DEPTH` with the
`h − b ≥ 20` form ([stake.h:73](src/modern/stake.h:73)), consumed by
[stake_registry.h:39](src/node/stake_registry.h:39).

---

## C-5 — "Freeze the corridor" does not close the corridor's OPEN list

**Severity: medium — a sequencing trap.**

Master §7 item 1: "Freeze the accepted transition corridor except for critical bugs."
Read alone, this suggests the corridor is finished. It is not:
[b3-during-fork-transition.md](b3-during-fork-transition.md) §12 still lists as OPEN —

- corridor difficulty policy and every numeric parameter;
- corridor reward model and the miner→validator-capture rule;
- **cutoff C** and the registration/burial split;
- insufficient-stake handling (options A–D);
- corridor reorg-depth bounds;
- STAKE serialization details and activation mechanics;
- minimum stake amount;
- duplicate/invalid validator-key handling;
- **initial randomness/VRF seed at M**;
- X-distribution operations (pause vs. precommit).

Three of these (**cutoff C**, **initial seed at M**, **insufficient-stake handling**)
are *inputs to* Modern PoS v1 — the registry that M consumes is defined by C, and the
first eligibility evaluation at M needs the seed. So build-order item 2 cannot
complete without closing part of item 1's OPEN list.

"Freeze" must be read as **no redesign**, not **no decisions remaining**.

---

## C-6 — PD-2 (validator key type) is already constrained by frozen serialization

**Severity: informational — but it means PD-2 is not a free choice.**

PD-2 offers "(a) BIP340 x-only Schnorr (32 B); (b) compressed ECDSA (33 B)."
The STAKE wire form already shipped with `STAKE_VALIDATOR_KEY_SIZE{32}`
([stake.h:67](src/modern/stake.h:67)), fixed inside `STAKE_PAYLOAD_SIZE` and matched
by `node::ValidatorKey = std::array<unsigned char, 32>`
([stake_registry.h:24](src/node/stake_registry.h:24)).

A 33-byte compressed ECDSA key does not fit without changing a serialization that
corridor STAKE outputs already use. Option (b) is therefore not cost-free; it is a
wire change. The owner should either lock (a) or explicitly accept the wire change.

---

## C-7 — Master §3.1's policy-name list vs. what exists in code

**Severity: low — factual drift only.**

Master §3.1 lists prototype policy names including `STAKE` and `BRIDGE`.
[policy.h:85](src/modern/policy.h:85) implements `LEGACY_LOCK=0, OWNER=1, BURN=2,
DEX_VAULT=3, STAKE=4`. **`BRIDGE` does not exist in code** — it is a design name
only. No renumbering is implied or permitted (contract §23; ratified deviation in
[b3-open-decisions.md](b3-open-decisions.md)).

---

## Agreements worth recording (checked, no conflict)

- **Master §2.5's premise is accurate.** The "existing mature `ValidatorRecord`
  registry" is real: [stake_registry.h:51](src/node/stake_registry.h:51),
  `StakeRegistry` at [:64](src/node/stake_registry.h:64), derived-only and
  recomputable from the UTXO set.
- **Per-validator weight aggregation** — master §2.4 and spec §5 agree, and the code
  implements it (`ValidatorRecord::total_weight` = sum of ACTIVE principal). Splitting
  a stake confers no advantage.
- **Timeline** — master §2.1 (`0..500` PoW, `501..H` legacy PoS, `H+1..H+1000`
  corridor, `M = H+1001`) matches the corridor document and PD-16 exactly.
- **Owner key ≠ validator key**, no auto-compounding rewards, no legacy coin-age —
  master §2.4 and spec §2 agree.
- **Master §9's superseded list** matches the repo's superseded records (OP_RETURN FN
  burn; the pre-H declaration-window bootstrap; the "every 25 FN → price doubles"
  curve rejected in OD-4).


---

## C-R1 — §4.5 marker-spend vs the integrated scan-and-claim — **RESOLVED**

The master handoff §4.5 captured the FIRST approved legacy-claim mechanism
(marker-based eligibility and LEGACY_LOCK marker consumption: its items 4, 6, 7,
8, 11; plus item 13's "configured" activation height). The owner's later explicit
correction — the integrated funding-key scan-and-claim recorded in commit
`750d983` ([b3-fn-pod.md](b3-fn-pod.md) §8) — supersedes that text per the
handoff's own precedence order:

- funding-key controllers authorize claims (fresh signatures over the
  chain-domain-bound claim message, one per distinct funding script);
- markers are audit metadata only — no ownership, no beneficiary, no
  eligibility role; the marker-spend design must not be implemented;
- one-claim-per-PoD is the reorg-managed `claimed[pod_id]` flag;
- activation is exactly the derived `M = TransitionPowFinalHeight(params) + 1`,
  with no independently configured FN activation parameter.

## C-R2 — §4.4 verification — **RESOLVED**

The historical PoD test correction is commit `d6a30ae`; the evolution suite
(`./build/bin/test_bitcoin --run_test=b3_evolution_tests`) is green as of
2026-08-16 (macOS arm64, Darwin 25.5.0, ~5m15s), covering every property §4.4
lists.

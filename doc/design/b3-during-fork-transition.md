# B3 during-fork transition — PROPOSAL (not locked)

**Status: PROPOSED — awaiting approval.** Captures the user's transition
model as clarified on 2026-08-15: the fork is not an instantaneous event but
a **process spanning the tail of the legacy era**, and the modern era still
begins at H+1. Nothing below is implemented.

## The model

    ... legacy era ... | H−W ....... DURING fork ....... H | H+1 ... modern era
                          declarations accumulate here       modern PoS starts
                          (legacy consensus, unchanged)      with a NON-EMPTY
                                                             validator set

- **Modern era from H+1** — exactly as the built machinery has it: era
  dispatch, codec hard-switch, boundary pins (H, X), mempool flush, reorg
  prohibition. None of that moves.
- **The during-fork window is `H−W .. H`** (working figure W ≈ 200; the value
  is DF-1, to be locked). The window is *inside* the legacy era: blocks are
  ordinary legacy blocks under unchanged legacy consensus, produced by the
  live legacy network. The chain never stops and there is no dead handover
  moment.
- **From H−W, stake declarations begin** ("force to policy"): holders lock
  value in transactions of a designated form that binds a validator key.
  To the legacy network these are ordinary, consensus-inert legacy
  transactions (old clients relay and mine them untouched). To the modern
  rules they are the **birth records of modern PoS stake**.
- **At H+1 the initial ACTIVE stake registry is derived from the window's
  declarations.** This is the same species of crossing support the
  architecture already commits to for ordinary value — pre-H UTXOs remain
  spendable post-H under their original conditions (LEGACY_LOCK) — extended
  with one more mapping: a pre-H *declared-stake* output crosses the boundary
  as a modern **STAKE** policy object instead of a plain LEGACY_LOCK. Since
  the boundary-crossing machinery must exist anyway, stake bootstrap rides
  it rather than needing any special modern-era mechanism.

## What this dissolves and what it preserves

- **PD-16 (H+1 bootstrap) is resolved by construction**: the modern era's
  first slot already has a deterministic, permissionless, weight-proportional
  validator set — everyone who declared in the window. The PoS spec's §10
  mechanisms (self-activating window, operator keys) become unnecessary.
- **The window is also the activation delay**: a declaration at H−k has k
  blocks of natural burial by H+1, replacing `N_activate` for the initial
  set (exact eligibility depth: DF-3).
- **Everything built on this branch survives unchanged**: trusted replay and
  the three-way U_master == U_port == U_replay framework attest the prefix
  through H *including the window and its declarations* (replay carries them
  mechanically like any transactions); the derived initial registry is
  therefore reproducible and attested by X. Era machinery, pinned-mode
  dispatch, the fail-closed modern gate: untouched.
- **The 2026-08-12 "hard-switch" record stands, reinterpreted precisely**:
  the *era/codec/consensus rule set* does switch at the boundary, as
  recorded and as built — but the *transition* is the window process
  spanning H−W .. H+1, and the modern era starts populated, not empty. The
  contract needs an **additive** amendment (the declaration window), not a
  boundary redesign.
- Precedent: the contract already commits to deriving a deterministic claim
  set from pre-H facts for FN claims (§49). Window-scoped stake declarations
  are the same pattern — additive derivation of modern state from a
  designated span of legacy history, never re-judging legacy validity.

## Decisions to lock (DF set — replaces PD-16)

- **DF-1 Window length W.** Working figure 200 blocks. Constraints: long
  enough for permissionless participation across time zones/outage windows;
  short enough that declared value's lock-before-fork exposure is
  acceptable. Simulation/coordination input.
- **DF-2 Declaration form.** The exact legacy transaction pattern: it must
  be valid, standard and relayable on the OLD network (consensus-inert
  pre-H), carry the validator key and the locked amount unambiguously, be
  recognizable by the modern client purely inside the window range, and be
  spend-protected until H (options: a script template the owner cannot
  cheaply respend before H vs. declarations voided if respent before H —
  the latter is simpler and self-policing: a respent declaration simply
  never crosses).
- **DF-3 Initial-set eligibility rule.** Which declarations are ACTIVE at
  H+1: declared at or before H−k (working figure: k as a fraction of W),
  unspent at H, amount ≥ the modern `MIN_STAKE_AMOUNT`. Deterministic from
  the attested prefix.
- **DF-4 Crossing mapping.** The precise STAKE-object form a declaration
  takes at H+1 (owner binding = the declaring output's legacy script
  commitment, mirroring LEGACY_LOCK; validator key from the declaration;
  amount = locked value), and its subsequent lifecycle (proposed: it enters
  the normal STAKE lifecycle — unstake requires the modern cooldown; no
  privileged status beyond having been born active).
- **DF-5 Fork-proceed condition.** Whether H is only finalized if the window
  accumulated a minimum total declared stake (the "enough validators to
  start" check), and how that interacts with choosing H/X operationally —
  natural fit: H/X are pinned after the fact anyway, so the operator
  observes the window succeeded before pinning.
- **DF-6 Window misbehavior.** Proposed: none before H+1 (the window is
  plain legacy consensus; validator obligations begin in the modern era).

## Effect on documents once approved

- Architecture contract: additive amendment defining the declaration window
  and the crossing mapping; the era boundary text is unchanged.
- PoS spec: §10 replaced by a reference here; PD-16 closed as "resolved by
  the during-fork declaration window"; remaining PDs unaffected; DF-1/DF-3
  join the simulation-gated numeric set.
- Open decisions: OD-1 remains open until the PoS PDs and this DF set are
  all locked.

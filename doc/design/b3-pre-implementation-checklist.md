# Pre-implementation checklist — corridor hardening before Modern PoS

**Status: CHECKLIST (2026-08-16). Nothing here is implemented by this
document.** Converts the design audit
([b3-corridor-design-audit.md](b3-corridor-design-audit.md)) into ordered,
checkable work items. Each item cites its audit finding, and is tagged
**[ENG]** (pure engineering, no open decision), **[DECIDE]** (needs an
explicit user lock first), or **[ENG after DECIDE]**. Explicitly out of
scope until this checklist is done and Modern PoS is specified: proposer
selection, VRF, PoS rewards, PoS fork choice.

## P1 — Phase-aware validation architecture (kill hidden binary assumptions)

The three-phase dispatch exists (`Consensus::GetConsensusPhase`); these
items make it the ONLY production-consensus selector, so no path silently
assumes "modern ⇒ SHA256d PoW" or "one boolean era."

- [ ] **P1.1 [ENG]** Restart index load: `LoadBlockIndexGuts`
  (`node/blockstorage.cpp:162`) — replace `era == MODERN ⇒ SHA256d
  CheckProofOfWork` with phase dispatch: TRANSITION_POW entries verify
  scrypt eligibility against their nBits; MODERN_POS entries keep the
  placeholder until the PoS header spec. Regression test with a
  non-trivial corridor target and a restart. (Audit N-1 — the one known
  real defect.)
- [ ] **P1.2 [ENG]** Headers pre-filter: replace the chain-wide
  `HasValidProofOfWork → true` B3 bypass with a phase-aware filter —
  corridor-range headers are header-only scrypt-verifiable once anchored
  (derive per-header heights inside a batch from the anchor); legacy-era
  headers keep the full-block deferral; post-corridor headers pass-through
  until the PoS header spec. Includes `CheckHeadersPoW` at
  `net_processing.cpp` and its interaction with `min_pow_checked`. (N-2.)
- [ ] **P1.3 [ENG]** Sweep every remaining era-boolean consumer and either
  convert to phase dispatch or record why two-valued is correct there:
  `SelectStakeRules` (modern/pos.h — needs the third phase),
  `GetBlockScriptFlags`/mempool era gates (correct two-valued: tx format
  follows era — document), miner era guard (already phase-aware),
  `needs_proof_of_work`-style guards, `ReadBlock` header-PoW guard
  (verified fine — pin with a comment/test). Deliverable: a table in the
  status doc, one row per consumer, phase-aware vs two-valued-by-design.
- [ ] **P1.4 [ENG]** Tests that fail on hidden binary assumptions: a
  corridor-configured chain with a NON-trivial corridor target must
  survive restart (P1.1), header-only relay of a corridor segment (P1.2),
  and reject SHA256d-ground corridor blocks whose scrypt hash fails.

## P2 — STAKE v1 canonical rules

- [ ] **P2.1 [DECIDE→lock, then ENG]** Minimal encoding: v1 REQUIRES the
  direct minimal push for the 38-byte payload; PUSHDATA1/2/4 encodings of
  a claiming payload are malformed claims (`bad-stake-output`). One stake
  datum ⇒ exactly one valid script byte-sequence. (S-1.)
- [ ] **P2.2 [DECIDE]** `MIN_STAKE_AMOUNT`: the constant must exist and be
  enforced at parse level (below it = malformed claim). The mainnet VALUE
  stays OPEN (economics/simulation); regtest scaffolding value explicit.
  (S-2.)
- [ ] **P2.3 [DECIDE]** Validator-key validity posture: choose one —
  (a) v1 keeps keys opaque and the PD-2 lock later defines invalid-key
  weight as excluded-from-eligibility-but-in-W; (b) excluded from both;
  (c) v1 pre-commits to a syntactic check now. Changes W at M; must be
  locked together with PD-2. (S-4.)
- [ ] **P2.4 [DECIDE→document]** Corridor cancellation vs production
  unbonding: LOCK the distinction explicitly — pre-M (corridor), cancel =
  ordinary owner spend, instant, no duties exist; from M, exit =
  unbonding with COOLDOWN. v1 carrier is thereby scoped CORRIDOR-ONLY for
  exits; the unbonding wire form (spend into a designated unbonding
  envelope vs a PoS-spec-defined mechanism) is a Modern-PoS-spec decision
  and must exist before M, not before the corridor. (S-3.)
- [ ] **P2.5 [DECIDE]** Degenerate owner scripts: forbid provably
  unspendable owner suffixes (leading OP_RETURN) as malformed claims, or
  allow at owner's risk with the dead-weight consequence documented; pin
  nested-claim (outer-wins) behavior in a test either way. (S-5.)
- [ ] **P2.6 [ENG]** Relay standardness carve-in for well-formed v1 STAKE
  creation transactions (policy, not consensus), gated to modern-era B3.
  (S-6.)
- [ ] **P2.7 [ENG]** The canonical `CTxOut → modern::ModernOutput` mapping
  function for STAKE (commitment = owner binding, params = key‖reserved),
  with round-trip tests — the single mapping later policy machinery uses.
  (S-7.)
- [ ] **P2.8 [ENG]** After P2.1–P2.5 land: freeze the carrier as
  "STAKE v1 FINAL (corridor scope)" in the transition doc, superseding
  the proposal-in-code caveat.

## P3 — Validator registry data model

Target: not aggregates, but enough for activation, exits, cutoff and
future slashing — while staying derived state, recomputable from the chain.

- [ ] **P3.1 [ENG]** Per-output records: registry entries carry
  (outpoint, validator key, amount, creation height, maturity status at
  the evaluation height); aggregation becomes a view over entries, not the
  stored form. (R-1.)
- [ ] **P3.2 [ENG]** Incremental, persistent maintenance: connect/
  disconnect deltas with crash-safe persistence (coinstatsindex-style),
  full-scan derivation retained as the audit/recovery path with an
  equality check between the two. (R-2.)
- [ ] **P3.3 [ENG after DECIDE]** Epoch/height snapshots: representation
  for "the registry as of height h" consumable at later heights (required
  by the epoch-freeze design); depends on P3.2's history. Snapshot
  CADENCE is a Modern-PoS-spec number — the mechanism is not. (R-3.)
- [ ] **P3.4 [ENG after DECIDE]** Cutoff C plumbing: `C` on params
  (mainnet value OPEN), registry output split into initial-ACTIVE
  (created ≤ C, unspent, mature) vs PENDING; e2e corridor test asserts
  the split. (R-4.)
- [ ] **P3.5 [DECIDE later, keep door open]** Slashing-readiness: entries
  must retain enough to attribute (validator key ↔ outputs ↔ owner
  binding) so a future evidence mechanism can locate and value a
  validator's stake; no slashing semantics now (PD-13 stays open) — just
  do not discard attribution data. (Cross-cut S-4/R-1.)

## P4 — Transition PoW economics analysis (analysis deliverable, then lock)

- [ ] **P4.1 [DECIDE]** Base reward model: fees-only / fixed subsidy /
  declining subsidy / PoS-fraction-priced (audit §3 a–d), decided with
  explicit issuance-cap accounting for any subsidy.
- [ ] **P4.2 [DECIDE]** Corridor coinbase maturity: an explicit corridor
  value (stock 100 is currently inherited implicitly).
- [ ] **P4.3 [DECIDE]** Miner→initial-set capture rule: whether corridor
  rewards can enter the initial validator set — options (i) mature beyond
  H+1000, (ii) spendable but initial-set-ineligible (provenance cost),
  (iii) stakeable only after M, (iv) unrestricted relying on cutoff C.
  Must be decided JOINTLY with C (P3.4) and the reward model (P4.1):
  with C ≤ H+1000−STAKE_ACTIVATION_DEPTH, any reward earned after
  C−STAKE_ACTIVATION_DEPTH cannot enter the initial set regardless.
- [ ] **P4.4 [ENG after DECIDE]** Wire the decided model into
  `transition_pow_reward`/maturity params + e2e coverage; document the
  cap accounting.
- [ ] **P4.5 [DECIDE]** Corridor difficulty policy (bits fixed vs simple
  responsive rule) — decided with N-3's low-claimed-work implications for
  header anti-DoS, `nMinimumChainWork` and assumevalid around the
  corridor.

## P5 — Phase-assumption audit of restart / sync / template / header paths

One pass, one table, every path tagged phase-correct or fixed:

- [ ] **P5.1 [ENG]** Restart: `LoadBlockIndexGuts` (= P1.1), block/undo
  read paths (`ReadBlock` verified fine — pin), `VerifyDB` over corridor
  ranges, reindex through H → corridor → gate.
- [ ] **P5.2 [ENG]** Sync: headers-first corridor relay (= P1.2), compact
  blocks over corridor blocks, orphan/unlinked handling around the
  boundary and corridor, anti-DoS work thresholds with corridor-scale
  claimed work (analysis feeding P4.5).
- [ ] **P5.3 [ENG]** Mining/template: `getblocktemplate` corridor
  annotation (which hash to grind, corridor bits/reward semantics) or a
  documented corridor mining interface; template validity self-check
  already phase-aware — pin with a test. (N-4.)
- [ ] **P5.4 [ENG]** Header rules table: for each of
  {context-free CheckBlockHeader, contextual header checks, GetNext*
  required-bits, timestamp rules} × {LEGACY_POS, TRANSITION_POW,
  MODERN_POS}: implemented rule, placeholder-or-final, test reference.
  Post-corridor rules remain the documented placeholder until the PoS
  header spec (N-5) — the table makes that explicit instead of implicit.
- [ ] **P5.5 [ENG]** Regtest e2e additions closing the loop: restart in
  mid-corridor and at H+1000; reindex across the whole corridor; sync a
  second (in-process) chainstate through the corridor from blocks.

## Sequencing

1. P1.1/P1.2 (the defect + header filter) and P5's audit table — no
   decisions needed.
2. P2.1–P2.5 decision batch (one user pass locks STAKE v1), then P2.6–P2.8.
3. P3.1/P3.2 engineering; P3.3/P3.4 after the snapshot-cadence and C
   decisions.
4. P4 decision batch (reward, maturity, capture, difficulty) — may need
   the simulation phase for numbers.
5. Only then: Modern PoS specification work (VRF, proposer selection,
   rewards, fork choice) against a hardened corridor.

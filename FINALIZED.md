# B3 — finalized state ledger

This branch (`finalized`) tracks what is FINALIZED. It is advanced only when
something moves from proposed/open to final. Working development continues on
`claude/b3-clean-architecture` (this tree) and, for the legacy reference
tooling, on `claude/b3-master-utxo-export` (the legacy `master` codebase).

Ledger advanced 2026-08-19 (owner-directed): corridor machinery, the legacy FN
issuance model, the FlowMesh deterministic engine corrections, and the
owner-accepted certified-log FlowMesh layer.

**Branch accuracy note (2026-08-19, post-Codex):** this branch reflects the
working tree as of merge `746cc72` and does NOT contain the subsequent Codex
repair passes (`ded7024`, `0098aeb`), which live on
`claude/b3-clean-architecture` pending review. The FlowMesh machinery
descriptions below therefore describe the PRE-repair implementation; consult
the working branch and its status matrix for the repaired state. FlowMesh code
is compiled into the node library but activation-unwired ("test-only" below is
the older, less precise phrasing). The approved spot matching model is the
uniform-price curve auction; price-time matching was never selected.

## Finalized — architecture

- One continuous chain; legacy era ≤ H attested by X, modern era from H+1;
  reorganizations across the boundary permanently prohibited; genesis
  permanent. Authoritative text: `doc/design/b3-architecture-contract.md`,
  under `doc/design/b3-master-handoff.md` as top authority (conflicts
  register: `doc/design/b3-master-handoff-conflicts.md`).
- Era/codec model: marker-driven codec and hash domain; era selection by
  height against the boundary; policy-output value model with append-only
  policy types.
- Trusted replay as the post-pin processing of attested legacy history
  (never re-judging live legacy rules), with live legacy consensus only
  while the boundary is unpinned.
- The migration acceptance invariant is three-way and row-diagnosable:
  `U_master(T) == U_port(T) == U_replay(T)`
  (`doc/design/b3-utxo-equivalence.md`), with the audited master-side
  membership predicate. No H/X pin and no startup replay wiring until it
  passes on real history at the three designated heights.
- **Transition shape (supersedes the declaration-window proposal): the
  temporary-PoW corridor** (`doc/design/b3-during-fork-transition.md`,
  authoritative 2026-08-16): H+1…H+1000 modern-format blocks whose
  production eligibility is B3's historical scrypt PoW while identity stays
  modern SHA256d; Policy Outputs active; `STAKE_ACTIVATION_DEPTH = 20`;
  per-validator weight aggregation; modern PoS from M = H+1001; legacy PoS
  never resumes after H.
- Modern PoS **design base** (accepted, details pending):
  `doc/design/b3-modern-pos-spec.md` — STAKE policy outputs; locked B3 as
  the only consensus weight; owner/validator key separation; rewards never
  auto-compound; bounded stake age; VRF eligibility with ranked fallbacks;
  cheap pre-verification; header unchanged. All PD numerics OPEN.
- **FN Coin model (owner rulings 2026-08-17/18):** ONE global chain-scoped
  fungible-but-indivisible colored asset (decimals 0, `MAX_FN_EVER_ISSUED =
  1000`, 1 unit per issuance); PoDId is an issuance nullifier, never asset
  or output identity; legacy issuance by the archival-builder /
  stateless-proof-verifier model (`doc/design/b3-legacy-fn-issuance-proposal.md`)
  — no production PodDB, no every-node rescan, no claim signatures.
- **FlowMesh architecture (owner-accepted 2026-08-19): a certified
  deterministic execution log**, not a second sovereign blockchain
  (`doc/design/b3-flowmesh-dex-decisions.md`): microblocks as the unit of
  batching/certification with certificate-separate identity; the settled
  curve-auction economics (persistent BID/ASK demand curves, canonical
  (signer, sequence, action_id) ordering, ONE maximum-volume uniform-price
  clearing pass per microblock, largest-remainder allocation) — a
  price-time order book is explicitly rejected; only FN Coin holders
  produce/certify microblocks; DEX fees in USDT (colored) to FN holders;
  deposits never trust caller-supplied facts (chain-verified at an explicit
  anchor); no custodian key can drain the vault. Owner-open items are the
  register's §3 list (seat lifecycle, fault bound/threshold, schedule
  ratification, anchor depth, deposit binding, withdrawal authorization,
  fee economics).

## Finalized — implemented and tested machinery

(on `claude/b3-clean-architecture`, each landed as a buildable commit; the
2026-08-19 gate: 191 B3-specific test cases across 29 suites, zero errors)

- Security blockers D1–D4, D7 (as before): cross-boundary reorg candidate
  rejection; membership-based legacy admission; anchor-ineligibility;
  bounded progress-safe legacy sync; bounded legacy orphanage; no
  process-global test hooks.
- Legacy correctness (as before): full live legacy consensus with the three
  sourced historical exception rules; checkpoints + depth bar mode-scoped;
  mempool era gate + boundary flush; trusted replay engine with undo
  emission, pinned-boundary dispatch and off-anchor recovery; the
  three-way equivalence framework and `b3coin-utxo-verify` operator tool
  with the smoke-verified legacy-side exporter.
- **Temporary-PoW corridor, regtest-complete:** `ConsensusPhase` dispatch;
  scrypt eligibility against constant corridor bits, fail-closed when
  unset; phase-aware production (assembler + regtest generate);
  LEGACY_LOCK crossing spends under frozen legacy rules; the STAKE `B3S1`
  script carrier and derived per-validator `StakeRegistry`; the full
  1,000-block corridor test ending at the fail-closed modern gate,
  including restart/reindex and two-node sync. Mainnet corridor numerics
  remain OPEN; mainnet H/X unset.
- **Legacy FN issuance machinery (activation-inert):** canonical merkle
  fold, strict proof codec, X-anchored stateless verifier with
  malleability-closed position rules, self-verifying archival builder and
  node-side sweep; PodDB with fail-closed derivation/rewind contracts as
  builder-side tooling only.
- **FlowMesh deterministic engine:** the D5 corrections (all passes) —
  credential-aware dedup, auth-before-equivocation, total curve
  evaluation, exact 128-bit interpolation, exact reservation
  consumption/release incl. the staircase bid bound, atomic SubmitCurve,
  canonical framed roots end to end, zero-price settlement.
- **FlowMesh certified-log layer (test-compiled only, wired into no
  consensus target):** copyable `FlowMeshState` with a pure canonical
  root and separate execution-result commitment; MB-0 atomic candidate
  execution; `MicroblockCoreV1` with bounded canonical serialization and
  certificate-separate tagged identity; BUY/SELL limit intents as
  degenerate curves; chain-fact-only DEPOSIT actions behind
  `DepositVerifier`/`AnchorRef` (fail-closed); BIP340 attestation
  certificates with the explicit fault-model threshold relation
  (2t−k>f uniqueness, t≤k−f liveness) and equivocation evidence; the
  minimal round/lock leader-recovery mechanism behind interfaces
  (round-robin schedule provisional); bounded deterministic action pool;
  transport-agnostic `MeshNode` (propose → independent re-execution →
  attest → certify → commit; catch-up; duplicate/garbage tolerance);
  durable `FlowMeshStore` certified log (atomic entry+marker appends,
  fully re-verifying replay) with certificate-authenticated snapshots
  (an unauthenticated snapshot can never become state);
  `ChainAnchorPolicy` with the OD-6 depth as an explicit owner input;
  Schnorr action credentials (domain-bound, account = tagged key hash);
  fuzz targets for every codec; root cost measured (128 µs @1k /
  1.3 ms @10k accounts — incremental commitment deliberately deferred).
  Verified: three-node convergence with a real cleared trade and
  withdrawal receipt; restart, snapshot restart and corruption fallback;
  lagging-node catch-up; vote-split round recovery without double
  finality; equivocating-proposer containment (k=4, f=1, t=3); anchor
  gating and B3-reorg interaction; FlowMesh-outage isolation from B3
  block production.
- Modern-layer correction: the STAKE policy-switch gap closed (structural
  STAKE v1 output rules enforced; proof layer explicitly fail-closed
  pending the unbonding decision).

## Not finalized (tracked, not on this branch's guarantees)

- Real-history three-way runs at the designated heights: the H/X
  activation gate (operator step).
- Modern PoS protocol details (OD-1): PD set OPEN; corridor OPEN numerics
  (difficulty, reward and capture rule, cutoff C, initial seed at M,
  insufficient-stake handling, `MIN_STAKE_AMOUNT`); the STAKE carrier
  awaits ratification before any mainnet H/X pin.
- FlowMesh owner decisions (register §3): FN seat lifecycle; fault bound f
  / certificate threshold; proposer-schedule ratification and timeout
  policy; B3 anchor finality depth and reorg posture; deposit account
  binding; trustless withdrawal authorization (receipt-vault
  ratification); fee rate and distribution.
- FlowMesh engineering deliberately parked: net_processing/RPC wiring
  (until seats exist); production deposit/withdrawal verifiers (until the
  base-chain policy/asset layer activates); incremental state root
  (until scale demands — bench is the tripwire).
- **Futures track (corrected 2026-08-19 per the Codex repair directive):
  the ONLY locked facts are "FlowMesh will support futures" and
  "maximum leverage = 10×". NOT implemented.** Perpetual vs dated
  futures, isolated vs cross margin, funding, the mark/index source,
  maintenance margin, liquidation penalties, an insurance fund and ADL
  are ALL open owner decisions; the in-session cascade design is a
  proposal record only, not accepted direction.
- Known open defects: txindex/txospenderindex hash-domain selection;
  `CDiskBlockIndex` format-version bump; corridor headers-first relay
  (`HasValidProofOfWork`) still SHA256d-assuming; the full-binary unit
  test cascade from the catalogued `bloom_tests` identity abort.

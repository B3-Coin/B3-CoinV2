# B3 — finalized state ledger

This branch (`finalized`) tracks what is FINALIZED. It is advanced only when
something moves from proposed/open to final. Working development continues on
`claude/b3-clean-architecture` (this tree) and, for the legacy reference
tooling, on `claude/b3-master-utxo-export` (the legacy `master` codebase).

## Finalized — architecture

- One continuous chain; legacy era ≤ H attested by X, modern era from H+1;
  reorganizations across the boundary permanently prohibited; genesis
  permanent. Authoritative text: `doc/design/b3-architecture-contract.md`.
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
- Modern PoS **design base** (accepted, details pending):
  `doc/design/b3-modern-pos-spec.md` — STAKE policy outputs; locked B3 as
  the only consensus weight; owner/validator key separation; rewards never
  auto-compound (consensus-enforced); bounded stake age; VRF eligibility
  with ranked multi-proposer fallbacks; cheap pre-verification; header
  unchanged.
- Transition shape: the fork is a process — modern era from H+1, with the
  during-fork declaration window in the legacy tail (proposal pending lock:
  `doc/design/b3-during-fork-transition.md`).

## Finalized — implemented and tested machinery

(on `claude/b3-clean-architecture`, each landed as a buildable commit)

- Security blockers D1–D4, D7: cross-boundary reorg candidate rejection;
  membership-based legacy admission without imposing PoW on PoS history;
  anchor-ineligibility with persisted status and best-header exclusion;
  bounded progress-safe legacy sync (owner lease, per-peer retry bar);
  bounded off-lock legacy orphanage; no process-global test hooks.
- Legacy correctness: full live legacy consensus (kernel, modifiers,
  rewards incl. the three sourced historical exception rules, difficulty,
  FN collateral); the 13 historical checkpoints + rolling depth bar,
  mode-scoped; mempool era gate + atomic boundary flush + provenance-
  carrying mempool.dat v3.
- Trusted replay engine with standard undo emission; pinned-boundary
  dispatch (admission, connect, read-back all replay-scoped when H/X are
  configured); off-anchor active-tip recovery (fake-work chain unwind).
- Equivalence framework: canonical logical UTXO rows (`b3-utxo-rows/v1`,
  byte-identical across producers), `UtxoSetCommitment`, the
  `b3coin-utxo-verify` operator tool with `-portrows/-replayrows/
  -masterrows` three-way verdict; the legacy-client reference exporter
  (`claude/b3-master-utxo-export`: exporter, arm64 build makefile,
  `-exportstopatheight` capture tooling) — compiled and smoke-verified,
  T=0 real-genesis three-way run EQUAL with byte-identical rows and both
  mutation-test outcomes exact.
- Integration coverage: full transition suite including the non-empty
  live-history transition test ending at the fail-closed
  `no-modern-pos-rules` gate at H+1.

## Not finalized (tracked, not on this branch's guarantees)

- Real-history three-way runs at T1=95350, T2=110000, T3 (blocked on chain
  data): the activation gate.
- Modern PoS protocol details: PD-1..PD-17 all OPEN (numerics simulation-
  gated); during-fork window DF-1..DF-6 PROPOSED.
- Modern PoS implementation (fail-closed gate stays), miner/submitblock
  marker-awareness, txindex hash-domain fix, activation plumbing,
  FlowMesh/FN/assets (gated behind a clean H+1).

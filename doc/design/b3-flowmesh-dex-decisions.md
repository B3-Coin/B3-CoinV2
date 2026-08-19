# FlowMesh microblock DEX — decision register (2026-08-19)

**Status: committed record, pending owner review.** Records the owner's
explicit rulings from the 2026-08-19 protocol-audit and build-out session, the
defaults adopted under the simplest-compatible rule, and the decisions that
remain OPEN. The rulings govern because the owner stated them directly (chat,
2026-08-19); this file is their durable record and acquires full documentary
authority once the owner has reviewed this commit. Where a ruling contradicts
an earlier document, the contradiction is listed in §4 for reconciliation
rather than silently resolved.

**Futures scope (CORRECTED per the Codex repair directive, 2026-08-19):** the
ONLY locked futures requirements are:

    FlowMesh will support futures.
    Maximum leverage = 10×.

Nothing else is decided. Perpetual vs dated futures, isolated vs cross
margin, funding, the mark/index source, maintenance margin, liquidation
mechanics/penalties, an insurance fund, and ADL are ALL OPEN owner
decisions. The perp/margin/liquidation material discussed in-session
(including the Hyperliquid-parity cascade) is a PROPOSAL record only and
must not be read as approved. No futures/leverage code exists.

---

## 1. Owner rulings (LOCKED, 2026-08-19)

| # | Ruling | Notes |
|---|---|---|
| L-1 | **Microblocks are FlowMesh's primary execution unit**, with speed comparable to Hyperliquid (sub-second cadence target). | Confirms the protocol brief §13 direction and adds a latency bar. |
| L-2 | **Only FN Coin holders may produce microblocks** and confirm FlowMesh trading transitions. | Matches brief §11. Whether bare holding suffices or a lock/activate-1-FN seat is required stays OPEN (O-1). |
| L-3 | **DEX fees are denominated in USDT** (a USDT-backed colored coin) **and collected by FN Coin holders.** | Matches brief §18. Distribution rule OPEN (O-4). |
| L-4 | **FlowMesh will support futures; maximum leverage = 10×.** | The complete locked futures statement. Everything else about futures is OPEN (see the corrected scope note above); the previously recorded "perpetuals" and "cross+isolated" entries are WITHDRAWN as over-claims per the Codex repair directive. |

## 2. Defaults adopted (REVISABLE, not protocol-locked)

| # | Default | Basis |
|---|---|---|
| D-1 | **THE approved spot matching model is the uniform-price curve auction** (persistent per-account BID/ASK demand curves, canonical (signer, sequence, action_id) execution order, exactly ONE maximum-volume uniform-price clearing pass per microblock, largest-remainder allocation). Price-time priority and a conventional order book are explicitly NOT selected and must not be introduced; BUY/SELL intents map onto degenerate curves preserving these economics exactly. (An intermediate same-day recording that price-time matching had been selected was an error, corrected by the Codex re-audit directive.) | Final settled state per the Codex repair directives, 2026-08-19. |
| D-2 | **Amount quantization instead of rounding**: order quantities are whole multiples of a provisional lot (`QTY_LOT`); price is quote units per lot, so every fill's quote leg is exact integer arithmetic with **no rounding rule at all**. The only floor division is the taker fee (`floor(quote · fee_ppm / 1e6)`). Lot/tick values per market are OD-7 (owner); the constants are provisional. | Simplest deterministic arithmetic satisfying the brief's "explicit overflow and rounding rules". |
| D-3 | **Action authorization**: BIP340 x-only Schnorr over a domain-tagged action digest, account id = tagged hash of the pubkey, behind a swappable authenticator seam. | Brief §15 requires reusing existing repo primitives (no BLS); BIP340 is in-tree. Seam keeps it replaceable if the owner rules otherwise. |

## 3. Open decisions (owner)

Ordered so that the earlier ones unblock the most work.

- **O-1 FN seat mechanism** — bare holding vs lock/activate 1 FN → 1 seat with a
  bound operator key (the brief §11 v1 direction). Locking keeps unlocked FN
  freely transferable and makes the active-operator set explicit state.
- **O-2 Proposer selection** among active seats (round-robin, stake-weighted,
  VRF, …), **committee size and certification threshold**. Now latency
  decisions as well as security decisions (L-1).
- **O-3 Microblock ↔ B3 anchoring and finality** — cadence of anchors, the
  finality a deposit/withdrawal must satisfy, reorged-anchor behavior
  (= OD-6 in [b3-open-decisions.md](b3-open-decisions.md)).
- **O-4 Fee distribution rule** — proposer takes its block's fees vs pro-rata
  across all active seats (affects proposer incentives; decide with O-2).
- **O-5 Deterministic mark price** for any future leveraged market — every validator must compute the
  identical price. Either derived purely from internal clearing state, or
  oracle inputs entering as ordered microblock actions (which makes the oracle
  a privileged actor to be specified). Hardest new decision L-4/L-5 creates.
- **O-6 Funding-rate rule** — formula, interval, clamps.
- **O-7 Liquidation mechanics** — trigger rule, who executes (keeper actions vs
  automatic engine step), penalty, and the insolvency backstop (insurance fund
  vs socialized loss). Cross-margin (L-6) requires whole-account atomic
  evaluation.
- **O-8 Per-market parameters** — price tick / lot / fee precision (= OD-7),
  per-market leverage caps within the global 10x.
- **O-9 Vault mechanism ratification** — the consumed-finalized-receipt keyless
  vault in `src/modern/vault.h` is implemented model-only and was never
  owner-ratified; deposits/withdrawals for the DEX depend on it (or a
  replacement).

## 4. Contradictions created (reconcile in reviewed commits)

- **Leverage scope, final state (Codex repair directive, 2026-08-19):** only
  "futures supported, max 10×" is locked. v1 ships spot-only; every other
  futures mechanic is an open owner decision and earlier same-day recordings
  claiming more (perpetuals, cross+isolated margin, liquidation cascade) are
  corrected as over-claims.
- L-4 reverses the 2026-08-19 audit brief's §15 ("no leverage initially, no
  liquidation system initially") and §23 (leverage/liquidations listed under
  "do not add yet") — resolved by the sequencing reading above.
- [b3-implementation-status.md](b3-implementation-status.md) §7 lists
  positions/margin/PnL as missing-by-scope for FlowMesh; with L-4/L-5 they are
  now required scope (still gated behind the contract's clean-H+1 sequencing
  for any consensus wiring).
- The master handoff's FlowMesh sections should gain the L-1..L-6 rulings when
  next revised; until then this register is the pointer.

## 5. Implementation status (2026-08-19, certified-log build-out)

Landed on `claude/b3-clean-architecture` (commits `bd80478..`; the FlowMesh
layer is COMPILED into the node library but ACTIVATION-UNWIRED — no consensus,
networking or RPC path reaches it):

- **FlowMeshState** value type (ledger + persistent curve book + signer
  sequences + consumed deposits) with a PURE canonical state root;
  candidate execution is copy-apply-commit (MB-0 atomicity).
- **Batch executor** preserved economics exactly — canonical
  (signer, sequence, action_id) ordering, ONE uniform-price clearing pass
  per microblock, largest-remainder allocation — plus DEPOSIT actions
  that carry ONLY an outpoint (facts come from a DepositVerifier at an
  explicit AnchorRef; fail-closed without one) and a separate
  ExecutionResultCommitment.
- **MicroblockCoreV1** (canonical serialization, tagged identity,
  bounds), ExecuteCandidate/BuildMicroblock, BUY/SELL limit intents as
  degenerate curves (order book explicitly not built).
- **Certificates** as separate objects: BIP340 attestations over a
  domain+sequence-bound digest, canonical assembly/verification against
  the seat set, MinCertificateThreshold exposing 2t−k>f / t≤k−f, and
  equivocation evidence.
- **Leader recovery**: minimal round/lock mechanism behind
  ProposerSchedule + AttestationGuard (RoundRobinSchedule provisional);
  not naive rotation, not an imported BFT stack.
- **MeshNode** orchestration + bounded ActionPool + catch-up protocol;
  **FlowMeshStore** durable certified log (atomic entry+marker appends,
  deterministic re-verifying replay); **ChainAnchorPolicy** (OD-6
  mechanics, depth as explicit owner input);
  UnavailableDepositVerifier keeps production deposits fail-closed.
- **Schnorr action credentials** (account = tagged key hash, domain
  bound, existing secp256k1 only).
- Tests: 49 FlowMesh cases incl. three-node convergence, restart/replay,
  catch-up, vote-split recovery, equivocation containment (k=4/f=1/t=3),
  anchor gating + B3-reorg interaction, outage isolation. The modern
  STAKE policy-switch gap is closed separately.

Additions after the initial build-out (same day):
- **Certificate-authenticated snapshots** (`cc9b10b`): canonical
  whole-state serialization + a store snapshot slot; write-time and
  load-time verification against the certified resulting_state_root at
  the snapshot sequence, strict decode, fallback to full replay — an
  unauthenticated snapshot can never become state.
- **State-root cost measured** (`a23244c`): 128 µs @ 1k accounts,
  1.3 ms @ 10k (flat framed root). Under 1% of a core at any realistic
  microblock cadence, so the incremental state commitment is
  **deliberately deferred** (avoid changing finalized constructions
  until necessary — owner guardrail 2026-08-19); the benches are the
  tripwire for revisiting.
- **Fuzz targets** (`49a0852`) for the action/microblock/certificate/
  snapshot codecs incl. candidate-execution atomicity on garbage.

Still open engineering (no owner decision needed): real P2P wiring of
the message layer into net_processing and an RPC surface — both parked
deliberately until the FN seat lifecycle (O-1) exists, since without
seats there is no network to join or expose; the production
DepositVerifier and withdrawal-authorization consensus wiring once O-9
and base-chain activation land; incremental state root only if scale
ever demands it (see above).

## 6. Build consequences (original plan, superseded by §5 where they overlap)

Layered so spot determinism lands first and perps reuse the same rails:

1. **Microblock object** (`src/flowmesh/`, header-only/test-only): serializable
   `{sequence, parent_microblock_id, b3_anchor, actions, previous_state_root,
   resulting_state_root, proposer, certificate}`; the current slot root does
   not commit its predecessor — fix as part of this.
2. **Incremental state commitment** — replace the flat full-state rehash
   (acknowledged O(state) per slot in `src/bench/flowmesh.cpp`) with an
   incrementally maintained root; this is the L-1 speed gate.
3. **Proposer/certificate seams** — `ProposerRule` / `CertificateVerifier`
   interfaces with test-only implementations, so the pipeline is testable
   before O-1/O-2 lock. FN-holder gating (L-2) plugs in here.
4. **Deterministic replay test** — N instances × same serialized microblock
   stream → byte-identical roots.
5. **Fee hook** — clearing charges a USDT-denominated fee (rate provisional,
   zero acceptable for tests) into per-seat accrual; distribution per O-4.
6. **Futures layer on the same rails** (after spot determinism; instrument
   form, margin modes, funding, mark price, liquidation and backstop are ALL
   open owner decisions — only "futures, max 10×" is locked). Structured as
   new action types and state under the same microblock/certification
   machinery when the decisions land.

Consensus wiring of any of this remains gated behind the contract's clean-H+1
sequencing and the O-9/OD-6 decisions; everything above is buildable and
testable header-only first, as the existing FlowMesh layer already is.

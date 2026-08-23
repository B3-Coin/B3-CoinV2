# B3 — finalized state ledger

This branch (`finalized`) tracks what is FINALIZED. It is advanced only when
something moves from proposed/open to final. Working development continues on
`claude/b3-clean-architecture` and, for the legacy reference tooling, on
`claude/b3-master-utxo-export` (the legacy `master` codebase).

Ledger advanced 2026-08-23 (owner-directed), at working-tree commit `923e989`.
Previous advances: 2026-08-22 (`65b426a` + `b283381`), 2026-08-19
(`746cc72` + `953c915`).

**Branch accuracy note (2026-08-22):** this branch now CONTAINS the Codex
repair passes (`ded7024`, `0098aeb`, `8c842ce`, `37e5386`), the adversarial
self-audit (`66a2d3b`), the Codex follow-up closures (`178e155`, `e9a52e4`)
and everything after them up to `923e989`. The earlier note that these were
missing is superseded. FlowMesh code is compiled into the node library and
remains activation-unwired (no consensus target, no net_processing/RPC).
The last recorded full B3 gate on the working branch is 222 cases green
(`7c6a72f`, colored assets v1); this ledger records that figure, it does
not re-run it.

## Finalized — architecture

- One continuous chain; legacy era ≤ H attested by X, modern era from H+1;
  reorganizations across the boundary permanently prohibited; genesis
  permanent. Authoritative text: `doc/design/b3-architecture-contract.md`,
  under `doc/design/b3-master-handoff.md` as top authority (conflicts
  register: `doc/design/b3-master-handoff-conflicts.md`; open decisions:
  `doc/design/b3-open-decisions.md`).
- Era/codec model: marker-driven codec and hash domain; era selection by
  height against the boundary; policy-output value model with append-only
  policy types; marker-aware index hash domain and transaction codec.
- Trusted replay as the post-pin processing of attested legacy history
  (never re-judging live legacy rules), with live legacy consensus only
  while the boundary is unpinned.
- **Migration acceptance invariant SATISFIED on real history (2026-08-22,
  owner-authorized live sync):** `U_master(T) == U_port(T) == U_replay(T)`
  verified EQUAL at T1 = 95,350, T2 = 110,000 and T3 = 797,000 (X
  cross-checked old-client-vs-explorer; mandatory mutation-negative
  caught). The H/X activation gate is satisfied; a final capture at the
  chosen H is part of the pinning procedure. Full chain preserved at
  height 807,709 with an offline bootstrap
  (`doc/design/b3-utxo-equivalence-runs.md`).
- **Transition shape: the temporary-PoW corridor**
  (`doc/design/b3-during-fork-transition.md`): H+1…H+1000 modern-format
  blocks produced under B3's historical scrypt PoW with modern SHA256d
  identity; Policy Outputs active; `STAKE_ACTIVATION_DEPTH = 20`;
  per-validator weight aggregation; modern PoS from M = H+1001; legacy PoS
  never resumes. **Corridor rulings 2026-08-21:** one fixed difficulty
  constant for the whole corridor, no retarget (value policy: low,
  stall-safety dominant, calibrated to one CPU core; exact bits measured
  at H/X pinning); reward RATIFIED 0 — fees only, fail-closed when unset;
  cutoff C ruled out of existence (the 20-block STAKE activation depth
  alone defines the initial ACTIVE set at M); no minimum-total-stake
  consensus gate; STAKE v1 `B3S1` carrier RATIFIED exactly as implemented.
- **Modern PoS V1 — mechanism FROZEN (owner rulings M1–M6, 2026-08-20),
  numerics ratified 2026-08-21** (`doc/design/b3-modern-pos-spec.md`):
  deterministic stake-weighted hash eligibility over a chained seed with
  saturating recovery rounds; exact deterministic timestamps encoding the
  round; BIP340 validator signatures over the domain-separated block hash;
  no unstake cooldown; PoS-native height/round fork choice with a fixed
  reorganization horizon; unconditional modern coinbase cap and
  reward-STAKE prohibition. RATIFIED numbers: block interval 60 s, round
  30 s, f0 = 1, ×2 relaxation, `MIN_STAKE_AMOUNT` = 333 modern B3 (kB3;
  333e9 base units, stated on mainnet, inert until H/X),
  `MODERN_REORG_HORIZON` D = 1440. Provisional (REVISABLE_BEFORE_MAINNET):
  sentinel bits, future-drift bound, modern reward (OD-2); the parameter
  block ships unset on every network until those are settled, so modern
  PoS still fails closed everywhere shipped. VRF/epochs/committees/
  slashing/finality gadgets/delegation are V2 research.
- **FN Coin model:** ONE global chain-scoped fungible-but-indivisible
  colored asset (decimals 0, 1 unit per issuance); PoDId is an issuance
  nullifier; legacy issuance by the archival-builder /
  stateless-proof-verifier model (no production PodDB, no every-node
  rescan, no claim signatures). **`MAX_FN_EVER_ISSUED` RATIFIED 5,000
  (2026-08-22)** after the equivalence-gated real-history report found
  R = 3,500 qualifying historical PoDs (all claimable) — every historical
  FN honored, 1,500 headroom; the earlier 1,000 is superseded.
- **Colored assets simple v1 (owner rulings 2026-08-22):** `AssetIdV1 =
  TaggedHash("B3/ASSET/V1") ‖ ModernChainDomain ‖ issuance_outpoint ‖
  H(genesis record)` — tagged, chain-bound, rule-bound (contract §21 reads
  as this form); asset-wide rules stated once in the issuance transaction
  as the immutable `AssetGenesisV1 {max_supply, decimals ≤ 18,
  issuance_mode, mode_params}`; v1 accepts only `GENESIS_FIXED` (genesis
  mints exactly `max_supply` once; conservation forbids later surplus);
  `AUTHORITY_MINT`, `POW_MINED`, `BRIDGE_BACKED` reserved and refused in v1.
- **FlowMesh architecture: a certified deterministic execution log**, not
  a second sovereign blockchain (`doc/design/b3-flowmesh-dex-decisions.md`):
  microblocks with certificate-separate identity; the uniform-price curve
  auction (persistent BID/ASK demand curves, canonical (signer, sequence,
  action_id) ordering, ONE maximum-volume clearing pass per microblock,
  largest-remainder allocation) — a price-time order book is rejected;
  only FN Coin holders produce/certify microblocks; deposits never trust
  caller-supplied facts. **Rulings 2026-08-22 made structural:** spot and
  futures are SEPARATE STATE DOMAINS (L-5; explicit `SPOT_TO_FUTURES` /
  `FUTURES_TO_SPOT` reserved and rejected in v1); `DEX_VAULT` policy v2
  with `USER_DEPOSIT` (credits exactly once) and `VAULT_POOL_CHANGE`
  (never a balance) — all outputs under one keyless vault policy; the
  keyless receipt-vault RATIFIED with the redeemability rule (certified
  AND anchor canonical AND buried ≥ depth AND receipt unconsumed; remainder
  → `VAULT_POOL_CHANGE`; consumed once); certificate finality YES — a
  conflicting valid certificate is evidence and a fail-safe halt;
  `FLOWMESH_ANCHOR_DEPTH = 30`, distinct from the 1440 horizon. Futures:
  the ONLY locked facts remain "supported, maximum leverage 10×".
- **Stablecoin and bridge rulings (2026-08-22):** FlowMesh's first real
  quote asset is the **B3-native overcollateralized (CDP-backed)
  stablecoin bUSD** (register L-6; design record
  `doc/design/b3-native-stable-proposal.md`) — collateral-backed, never
  seigniorage; FN-certified oracle as the single trust point; formula-sized
  debt ceiling; custody in the keyless DEX vault; CDP as a third state
  domain; B3-only collateral at launch, BTC added when SPV peg-in exists.
  **No bridge, federation or external issuer is a dependency of FlowMesh**;
  the fiat path is external B3 markets. **L-3 AMENDED:** DEX fees in an
  approved dollar-stable `AssetId` collected by FN holders — bUSD first,
  bridged USDT/USDC addable only by explicit ruling, never by ticker.
  **OD-8 direction:** any protocol-level bridge is light-client/SPV on the
  mint leg (Ethereum sync committee, Bitcoin SPV), never a signer set in
  consensus; one bridged-asset policy with a rotatable signer set so
  managed → outsourced → light-client → issuer-native are in-place
  transitions; Tron and similar served off-consensus; native issuance as
  the end state. (Ethereum L1 as origin is recommended, not ruled.)
- **Release-v1 rulings (owner, 2026-08-23)** — recorded in
  `doc/design/b3-release-v1.md`, OD-10 and the corridor document:
  **H = 820,000**, corridor 820,001..821,000, **M = 821,001**;
  **X distribution = PAUSE, FAIL CLOSED** — a release ships with H set and
  X blank, accepts the chain through H and refuses every block above H
  (no-penalty, nothing marked invalid; production refuses; startup
  warning), the mandatory follow-up pins X, blank-X nodes never enter the
  corridor; **corridor bits = canonical compact `0x1f008000`** (same 2^239
  target as the non-canonical `0x20000080`; non-canonical configured bits
  fail closed); **corridor pacing = minimum 60 s spacing + 120 s future
  bound** (ruled after the verification that a fixed difficulty alone lets
  hashpower compress the corridor; the corridor now takes ≥ ~16.6 h of real
  time regardless of hashpower); seeds: `176.31.13.198` now, at least two
  more independently hosted fixed seeds and an owner-controlled DNS seed
  before the final release, no explorer peers without operator approval;
  **validator UX v1** = wallet-held validator key, `createstake`, staking
  status, start/stop staking, PENDING/ACTIVE visibility, automatic staking
  loop — nothing more advanced. **Pin gates (owner):** no final activation
  parameter is pinned or published until the live-sync legacy-mempool
  assertion bug is fixed, the T3 and final-H equivalence captures pass,
  seed infrastructure is operational, and release binaries are reproducible
  and audited — mainnet `hard_fork_height` / `legacy_final_hash` /
  `transition_pow_bits` remain unset (guard-tested).
- **Product: B3 Hive** (owner, LOCKED 2026-08-22) is the name of the
  release-v1 hard-fork client; FlowMesh remains the DEX engine's name.
  Release v1 = legacy continuity + corridor + Modern PoS V1, with FN,
  colored assets and FlowMesh shipped activation-inert behind later
  heights (`doc/design/b3-release-v1.md`).

## Finalized — implemented and tested machinery

(on `claude/b3-clean-architecture`, each landed as a buildable commit)

- Security blockers D1–D4, D7: cross-boundary reorg candidate rejection;
  membership-based legacy admission; anchor-ineligibility; bounded
  progress-safe legacy sync; bounded legacy orphanage; no process-global
  test hooks.
- Legacy correctness: full live legacy consensus with the three sourced
  historical exception rules; checkpoints + depth bar mode-scoped; mempool
  era gate + boundary flush; trusted replay engine with undo emission,
  pinned-boundary dispatch and off-anchor recovery; the three-way
  equivalence framework and `b3coin-utxo-verify` operator tool — now
  proven on real history (above). `Coin::nHeight` construction asserts and
  the 30-bit `MEMPOOL_HEIGHT` fit.
- **Temporary-PoW corridor, regtest-complete and ratified in code:**
  `ConsensusPhase` dispatch; scrypt eligibility against the constant
  corridor bits, fail-closed when unset; reward fail-closed when unset,
  mainnet 0 explicit; phase-aware production; LEGACY_LOCK crossing spends
  under frozen legacy rules; the STAKE `B3S1` carrier and derived
  per-validator registry; the full 1,000-block corridor test incl.
  restart/reindex and two-node sync; the header-spam pre-filter restored
  for marker-modern headers.
- **Modern PoS V1, implemented:** one parameter block
  (`modern_pos_params.h`, REVISABLE_BEFORE_MAINNET, guard-tested unset on
  every shipped network); header rules (sentinel nBits, zero nonce, exact
  timestamps decoding the round, pacing gate, horizon refusal); connect
  rules (seed chain, stake-weighted eligibility with recovery rounds,
  BIP340 validator signature, unconditional coinbase cap, reward-STAKE
  prohibition); `node::StakeTracker` (incremental, reorg/restart rebuilt);
  PoS-native fork-choice comparator; deterministic block production;
  regtest scenarios; chain-aware `submitblock`/proposal decoding.
- **Legacy FN issuance machinery (activation-inert):** canonical merkle
  fold, strict proof codec, X-anchored stateless verifier with
  malleability-closed position rules, self-verifying archival builder and
  node-side sweep; PodDB as builder-side tooling only; the equivalence-gated
  `-podreport` over real history.
- **Colored assets v1 code:** `AssetGenesisV1` creation action, bounded
  `mode_params`, explicit-issuance conservation rule (fail-closed while
  unpinned), shared `modern/chain_domain.h`.
- **FlowMesh deterministic engine and certified-log layer** (compiled,
  activation-unwired): the D5 corrections; `FlowMeshState` with canonical
  root and separate execution-result commitment; `MicroblockCoreV1` with
  certificate-separate identity; BUY/SELL as degenerate curves;
  chain-fact-only DEPOSIT behind `DepositVerifier`/`AnchorRef`; BIP340
  attestation certificates with the explicit fault-model threshold
  relation and equivocation evidence; round/lock leader recovery; bounded
  action pool; transport-agnostic `MeshNode`; durable `FlowMeshStore`
  certified log with authenticated snapshots; `ChainAnchorPolicy`; Schnorr
  action credentials; codec fuzz targets. **Plus the repair passes now on
  this branch:** the Codex audit/re-audit/surgical/boundary-hardening
  passes, the adversarial self-audit, the follow-up boundary closures and
  the four-blocker closure (reachable bid-residual bands; anchors rechecked
  immediately before every signature; store-neutral startup; futures text
  de-normativized); the structural 2026-08-22 rulings (spot/futures
  domains, `DEX_VAULT` v2 strict params, certificate-conflict halt,
  anchor depth 30). Verified: three-node convergence with a real cleared
  trade and withdrawal receipt; restart/snapshot/corruption fallback;
  catch-up; vote-split recovery; equivocating-proposer containment;
  anchor gating and B3-reorg interaction; FlowMesh-outage isolation.

- **Release-v1 implementation (2026-08-23):** `Consensus::LegacyBoundaryHeightOnly`
  and the `legacy-boundary-unpinned` header refusal + production guard +
  `LEGACY_BOUNDARY_UNPINNED` warning; `IsCanonicalCompactBits` enforced on
  the corridor constant (validation and production); the corridor pacing
  rule (`transition_pow_min_spacing = 60`, `transition_pow_max_future =
  120`; `time-too-early-corridor` / `time-too-new`; template time
  `max(now, parent + 60)`); the owner-supplied seed; **validator UX**: the
  wallet validator key (non-active `pk()` descriptor, `b3validatorpubkey`
  record), STAKE carrier resolved to its bare owner script for
  standardness / ownership / signing (`modern::StakeOwnerScript`, the relay
  carve-in; non-bare owner suffixes unsolvable), STAKE outputs excluded
  from automatic coin selection, `node::StakingLoop` behind
  `interfaces::Chain` (start/stop/status), wallet RPCs `createstake`,
  `getstakinginfo`, `startstaking`, `stopstaking`; the mainnet checkpoint
  table moved into `bitcoin_common` so `b3coin-tx` / `b3coin-util` /
  `b3coin-wallet` link. Verified: `modern_pos_tests` 14/14 (X-pause,
  canonical bits, pacing, staking loop, the V1 scenarios), the corridor
  transition cases incl. the 1,000-block corridor, restart/reindex and
  two-node sync, `b3_evolution_tests`, `fn_pod_tests`, `wallet_tests` incl.
  the validator-key/stake test, wallet/script regression suites; regtest
  smoke of the RPCs (key persists across reload, loop start/stop).

## Not finalized (tracked, not on this branch's guarantees)

- **Release v1, still owed** (`doc/design/b3-release-v1.md`): the pin
  gates themselves (live-sync assertion fix; T3 + final-H captures; two
  more fixed seeds and the DNS seed; reproducible audited binaries);
  platforms; regtest override flags + functional tests of the transition
  and staking RPCs; `getblocktemplate` corridor support; disk-format
  detection/reindex message; "B3 Hive" version strings; operator runbook.
  Mainnet H/X/bits remain unset until the gates pass.
- Modern PoS V1 provisional numerics: sentinel bits, future-drift bound,
  modern reward schedule (OD-2); the parameter block therefore still ships
  unset. V2 research items remain research.
- FlowMesh owner decisions still provisional under the 2026-08-20
  solve-don't-ask mandate (register §3b; staging/testnet only): O-1 seat
  mechanism, O-2 proposer/committee/threshold, timeout policy, O-4 fee
  distribution, O-9a/O-9c, OD-7 precision. Parked engineering:
  net_processing/RPC wiring, production deposit/withdrawal verifiers,
  incremental state root.
- **Futures:** only "supported, max 10×" is locked; NOT implemented; every
  mechanic open (O-5..O-8).
- **bUSD (L-6):** direction ruled; every parameter and mechanism detail
  OPEN (register O-10 / proposal §7: oracle parameters, MCR/CCR, ceiling
  formula, fees, rewards, bad-debt order, CDP-domain confirmation, the
  oracle+liquidation engine resequencing ahead of futures). No code.
- **Bridges (OD-8):** direction ruled; light-client verification rules,
  the `blst`/keccak/RLP/MPT dependency decision, thresholds, caps,
  watcher veto, upgrade/re-bootstrap rules, issuer-freeze handling, and
  L1-vs-L2 origin all OPEN. Behind step 12 / A3. No code.
- Colored-asset open items (handoff §10): activation heights, generic
  issuer authority/revocation, metadata rules, the reserved issuance modes.
- Known open defects / follow-ups: `CDiskBlockIndex` appended fields with
  no format-version bump (reindex required; detection/message planned for
  release v1 Day 2); the mid-sync legacy-mempool assertion incident
  (tracked fix; `-blocksonly` workaround); the full-binary unit-test
  cascade from the catalogued `bloom_tests` identity abort.

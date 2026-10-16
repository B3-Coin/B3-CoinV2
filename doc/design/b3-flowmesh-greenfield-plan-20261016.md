# B3 FlowMesh from scratch: the owner plan

**Date:** 2026-10-16. **Status:** PROPOSED plan. Nothing here is ruled, built or committed.
**Audience:** the owner (decisions, risks, calendar) and Codex (precise specs, exit gates).
**Inputs:** `greenfield/requirements.md` (full baseline, about 180 requirement IDs), `synthesis.md`, `design-{A,B,C}*.md`, `research-*.md`, and the three adversarial critiques (`critique-security.md`, `critique-performance.md`, `critique-feasibility.md`). Governing sources: the hl worktree `/Users/josh/development/ON/B3-FlowMesh-hl` at 2e44cf96, and `flowmeshV2-dev` at a64ea997 for `doc/design/b3-flowmesh-clean-core-20261016.md` (cited as [CC]) and `src/flowmesh2/C1_RESULTS.md`.

Every blocking critique is applied. Appendix A shows exactly how, and lists each should-fix item as accepted or deferred.

---

## 0. How to read this plan

**Labels.**

| Label | Meaning |
|---|---|
| LOCKED | A reviewed invariant or pinned constant. Changing it needs a new owner ruling plus a reviewed reconciliation commit. |
| RULED | An explicit, dated owner decision. |
| PROPOSED | A default this plan recommends. It is not valid for mainnet until the owner ratifies it. |
| OPEN | Undecided. The design leaves a seam and picks no value. |
| **RE-RULE** | This plan departs from LOCKED or RULED text. The owner must rule, and the governing documents must be reconciled in a reviewed commit before code depends on it (CLAUDE.md). |
| [inf] | Inference or model estimate, not a measurement or a source statement. |
| PLAN-n | A new requirement introduced by this plan (listed in §2.2). |

**Plain-language terms.**

| Term | Meaning |
|---|---|
| B3 chain | The one B3 blockchain: legacy history to block 810,000, modern PoS after it. |
| `b3d` | The B3 node: Bitcoin Core 31.1 plus the frozen B3 rules. It is the only code that holds `cs_main` (Bitcoin's global chain lock). |
| FlowMesh | The trading layer. It is not a separate chain. It settles to B3. |
| Seat | One FN-v2 output with a BLS key. One seat has one vote in FlowMesh. |
| Slot | One FlowMesh batch of actions (a "microblock"). Slots are event-driven: no fixed tick. |
| Certificate | An aggregated BLS signature from a quorum of seats. Quorum = ⌊2k/3⌋+1 of k seats. |
| Checkpoint | A FlowMesh state summary written into a B3 block. Withdrawals are paid only against accepted checkpoints. |
| Anchor | The B3 block a slot builds on. Default: finalized and at least 30 blocks deep. |
| Barrier | A forced write to disk (`fsync`). Each costs 0.05–0.2 ms on a server SSD with power-loss protection (PLP), and about 5.5 ms on the owner's Mac. |
| δ, δ_q | δ = one-way network delay. δ_q = time until the quorum-th fastest seat's message arrives. |
| p50 / p95 / p99 | Median / 95th / 99th percentile latency. |
| R1 / R2 / R3 | R1 = time from signing to a durable, client-verified result. R2 = same, for an actual fill with exact fees and balance changes. R3 = from verified result to the screen. |
| Q | The quote stable of V2 markets: bUSD on mainnet, rUSD on regtest. |
| Sentry | A public node that serves the API and relays actions. Seats never face the public. |
| DST | Deterministic simulation testing: the whole cluster runs in one process from a seed, so any failure replays exactly. |
| Twins | A test where two copies of one seat run with the same key, to simulate a lying seat. |
| P, C | Two future activation heights. **P** activates FlowMesh V2 on B3. **C** activates the base-chain finality/PoS improvements. They are independent. |

---

## 1. Summary

### 1.1 What we build

A new trading core for B3, designed from scratch, on the **same B3 chain** (Path A; owner to confirm, decision G0).

- **New, from scratch:** the FlowMesh V2 core (`src/flowmesh2/`, library `libfm_core`). It has one deterministic state owner, a BFT agreement layer, a crash-safe commit log, a seat mesh, a public API, a typed client verifier (`b3verify`), and a new trade page in B3 Hive.
- **Kept, not rewritten:** the B3 node (`b3d`). It keeps Bitcoin Core 31.1 validation, the frozen legacy rules, the sealed transition at 810,000, PoS V1, the M7 BLS finality gadget, assets, FN and the bridge light client.
- **One narrow seam:** a chain adapter. FlowMesh reads finalized B3 facts (deposits, seat changes) through it. FlowMesh writes certified checkpoints back to B3 as transactions. Nothing else crosses.
- **One new consensus activation on the DEX path:** height **P** adds `CheckpointV2`, `VaultV2`, B3-checked conservation, withdrawal limits and the pause record. The riskier base-chain work (finality V2, PoS V2, height **C**) runs as a separate track and no longer blocks the DEX.

### 1.2 Why from scratch

The current FlowMesh V1 was bolted onto Bitcoin's node. Audits found the limits are architectural, not tuning:

- Trading waits on Bitcoin's single global `cs_main` lock, and history-sized rebuilds run under it (a 97 s gap was observed).
- About 180 plain `std::mutex` uses are invisible to lock checking.
- Matching is one curve per account. Adversarial clearing takes 1.26–1.43 s at 4,096 curves.
- Measured result today: **258–263 ms** for one fill on the Mac with full fsync, and **1.4–2.4 s** for a burst of 8 (`audit-codex-overnight-20261016/audit-report.md` §1).

The owner ruled on 2026-10-16: "redesign completely, from scratch, using useful Bitcoin components and a modern C++ architecture … build alongside; do not delete history" ([CC]).

### 1.3 What we reuse from Bitcoin

- **Linked unchanged:** libsecp256k1 (BIP340), SHA-256 with hardware dispatch and tagged hashes, CRC32C, LevelDB, the `sync.h` annotated locks, `CConnman`/addrman for B3 P2P, the descriptor wallet and PSBT.
- **Copied with stricter limits:** `serialize`/`streams` with size caps, `uint256`, exact-amount and overflow helpers, `CCheckQueue` (parallel signature checks), `CuckooCache` (verify each action once), `flatfile` and `FileCommit` (crash-safe segment files), the BIP324 cipher for the seat mesh.
- **Patterns:** BIP152 compact short IDs for proposals, the ZMQ `sequence` cursor for gap-free streams, median-time-past (BIP113) if time-based expiry is ruled later.
- **Process:** the fuzz framework, sanitizers, functional tests and Guix reproducible builds.

§4 lists every component with its upstream path.

### 1.4 Headline numbers

| Item | Value |
|---|---|
| Ruled target (PERF-3) | Co-located R1 p50 ≤ 200 ms and p95 ≤ 600 ms, Linux, full fsync, n ≥ 1,000, failures counted |
| Model, co-located, idle, 64 seats | **≈15–18 ms p50** [inf]; p95 30–60 ms once the block-arrival stall is fixed (PLAN-11) |
| Model, co-located, maximum batch (x86) | ≈27 ms p50 with the 8,192-leaf cap [inf] |
| Model, regional US+EU seat core | ≈225–275 ms p50 [inf] |
| Model, global 64 seats | ≈450–600 ms p50 [inf] |
| Remote client | add the client's own round trip: ≈265–280 ms at 250 ms RTT [inf] |
| Owner's Mac, single host, strict barriers | ≈79 ms p50; ≈60 ms with barrier folding [inf] |
| First tester DEX on regtest ("DEX-1") | **≈7.5–9 months** [inf] |
| Mainnet activation decision | **≈18–24 months** [inf] |
| New C++ | ≈60–90k lines [inf] |
| Owner rulings needed before code beyond M1 | 9 (Tier 1, §12.1) |

"Hyperliquid-comparable" is reachable **when seats are co-located**. A decentralized worldwide seat set cannot reach a 200 ms median; physics sets ≈450–600 ms. The owner must rule on seat geography (§12.2, D-G1). No tuning removes this.

### 1.5 What changed after review

- **Custody is now honest.** Two-thirds of registered seats could move value up to the limits. The plan discloses this (RE-RULE of SEC-1) and adds B3-checked conservation, a pause window, a seat floor, seat activation pacing and limits sized to attack cost (Appendix A, B1).
- **Accounts cannot be frozen.** Strict per-account sequence numbers are replaced by a 100-entry nonce window, every action has a mandatory expiry, and cancels address order IDs (B2).
- **The latency gate now tests the real deployment.** New gate profiles cover load, block arrival, faults and WAN, with per-leader reporting (P-1). A new block no longer interrupts trading (P-2). Proposals fit the leader's upload link (P-3).
- **The DEX no longer waits on a base-chain hard fork.** Finality V2 and PoS V2 move to a separate track (F1).
- **Rulings are tiered.** Only 9 decisions are needed now (F2).
- **Futures need a named external oracle source before design starts.** There are no B3/Q futures until external liquidity exists (F3).
- **Scope is cut for DEX-1.** PBFT replaces Simplex. One process is used, not two daemons. Two committee tiers are not built. SSE replaces WebSocket.

---

## 2. Requirements baseline

### 2.1 Requirements (condensed)

The full list, with dates, is in `greenfield/requirements.md` §2. Source keys follow that file's §0. For example, AC = `doc/design/b3-architecture-contract.md`, HO = `b3-master-handoff.md`, [m:x] = memory file x.

| ID | Requirement | Status | Source |
|---|---|---|---|
| INV-1 | One continuous chain; history through X at H is immutable | LOCKED | AC §0–1; HO §1.1 |
| INV-2 | Genesis `4b0d7f13…983b6a` permanent; magic `b3 2e 1e e6`, port 5647 | LOCKED | AC §68; CL rule 4 |
| INV-3 | No new genesis, no balance migration, no UTXO snapshot | LOCKED | AC §1, §13 |
| INV-4 | Historical txids/outpoints/scripts byte-exact; `LEGACY_LOCK` spends stay valid | LOCKED | HO §2.3; AC §14–16 |
| INV-5 | H = 810,000; X = 2413ba59…d865c6; M = 811,001; corridor rules; no reorg across H | LOCKED | OD; IS; AC §2–3 |
| INV-7 | Destroyed value never returns; FN never becomes PoS weight | LOCKED | [m:b3-root-concept] |
| INV-8 | Roles never merge: STAKE secures, FN operates FlowMesh, assets trade | LOCKED | HO §1.2 |
| INV-9 | Deterministic: no floats, wall clock, unordered iteration in consensus or execution | LOCKED | HO §5.5; AC §36–37 |
| INV-10 | Never settle an OPEN parameter for convenience; report contradictions | LOCKED | HO §11; CL rule 1 |
| INV-12 | Policy Outputs only; params ≤ 80 B; policy and MPA numbers append-only | LOCKED | AC §23 |
| INV-13 | Microblocks are not a sovereign chain; inclusion ≠ certification ≠ B3 finality, labelled in every UI | LOCKED | HO §6.1, §6.6 |
| INV-14 | Old `wallet.dat` restores and spends after H | LOCKED | AC §65–67 |
| DEN-1 | 1 B3 = 1 kB3 = 1e9 base units | RULED 2026-08-17 | [m:b3-denomination-kb3] |
| DEN-4 | FN 0 decimals; bUSD and rUSD 6 decimals | RULED | OD-8; [m:b3-v2-stable-quote-ruling] |
| ECO-1 | R0 = 19,836,712,254 base units, halving every 525,600 blocks | RULED, pinned | OD-2 |
| ECO-2 | Subsidy 90% producer / 10% treasury | RULED 2026-08-26 | OD-2 |
| ECO-3 | Treasury is one single-key address `SNyANHiU…jLX` | RULED 2026-08-26 | ECO |
| ECO-5/6 | Base-chain fees in native B3 only; liveness never depends on a stablecoin | LOCKED | HO §3.5; AC §44 |
| ECO-7 | Trading fee 100 ppm once, 80% seats / 20% treasury; V2 in the quote stable; incidence OPEN | RULED / OPEN | ECO; [m:b3-v2-stable-quote-ruling] |
| ECO-8/9 | Issuance fee 1,000 B3; listing fee 100,000 B3; both to the treasury | RULED | ECO; [SQ] §1.7 |
| FN-3/4/5 | FN cap 5,000; FN Genesis R = 3,592 at block 810,001; cost curve 15k/30k/60k per 500, capacity 1,408 | RULED, pinned | OD-4; FNA; [m:b3-fn-cost-curve] |
| FN-9 | Only FN holders operate FlowMesh; one seat = one PoP-verified FN-v2 BLS key = one vote | RULED | DEX L-2; V1P §5 |
| FN-10 | Committee = all active seats, k ≥ 4, quorum ⌊2k/3⌋+1; v1 proposer round-robin | RULED (v1) | V1P §7 |
| FN-12 | Seat rewards claimable by the historical BLS key | RULED v1 / PROPOSED V2 | V1P §8 |
| AST-1 | Simple-v1 `GENESIS_FIXED` assets; AssetIdV1 preimage | RULED | OD; FNA §2 |
| AST-4 | Exact AssetId, never ticker | LOCKED | AC §22 |
| AST-5 | V2 AssetId commits to a rules-document hash | RULED in principle | [SQ] §1.5 |
| AST-6/8 | Listing by 100k B3 fee; removal by FN-seat vote; only after a working DEX | RULED | [SQ] §1.7 |
| AST-9/10 | No USDC; an overcollateralized coin is a separate product | RULED (R2, R7) | BRR |
| AST-12 | No VMs or PoW-mined assets in v1 | LOCKED | HO §6.9 |
| TRD-1 | Account model for trading; UTXOs only at custody entry and exit | LOCKED | HO §5.1 |
| TRD-2 | Keyless custody; permissionless relay; one-time receipts | LOCKED | HO §5.7–5.8 |
| TRD-3 | Deposit credited once per outpoint, only after finality | LOCKED | HO §5.2 |
| TRD-4 | Anchor depth 30 | RULED 2026-08-22; change PROPOSED (C-9) | OD-6 |
| TRD-5 | Certificate finalizes; conflicting certificate = evidence + halt; no automatic rollback | RULED | DEX O-9b |
| TRD-7 | v1 matching = uniform-price curve auction; price-time forbidden (D-1) | RULED (v1) | DEX D-1 |
| TRD-8 | V2: independent orders, IOC with slippage bound, post-only; HL-style book requested | OPEN (R-D3) | RP §4; owner 2026-09-30 |
| TRD-9 | V2 markets B3/Q and ASSET/Q; Q = bUSD (mainnet), rUSD (regtest) | RULED 2026-09-30 | [SQ] §1.1 |
| TRD-10 | One shared spot balance per (account, AssetId); atomic cross-market reservations | RULED requirement | S0 §1; RP §1 |
| TRD-12 | Futures supported, max 10× | LOCKED | DEX L-4 |
| TRD-13 | Spot and futures separate; explicit transfers only | RULED (L-5) | DEX L-5 |
| TRD-14 | Never a spot-only V2 release | RULED requirement (conflict C-5) | S0 §0; [CC] |
| TRD-16 | Do not start with cross margin, funding or ADL; last trade is never the mark | LOCKED | HO §5.11 |
| TRD-17 | BIP340-signed actions; authenticate before dedup | RULED / LOCKED | DEX D-3 |
| TRD-18 | Strict per-account nonce | LOCKED → **RE-RULE** (PLAN-1) | AC §38 |
| TRD-19 | Order expiry needs certified time or height | OPEN → PLAN-1 default | [HL] R14 |
| TRD-20 | Encrypted order flow deferred until an MEV analysis | LOCKED (deferral) | AC §41 |
| PERF-1 | Microblocks at Hyperliquid-comparable speed | RULED 2026-08-19 | DEX L-1 |
| PERF-2 | A latency target is never consensus | LOCKED | HO §6.8 |
| PERF-3 | Co-located p50 ≤ 200 ms, p95 ≤ 600 ms; no WAN gate yet | RULED target | S0 §13; [LAT] |
| PERF-4 | M1 definition (client t0 to verified certified inclusion) | LOCKED | [LAT] |
| PERF-5 | R1/R2/R3 definitions | PROPOSED; [CC] adopts R1/R2 | RP §6; [CC] |
| PERF-6/7 | n ≥ 1,000, failures counted, 60 s deadline, full fsync, mandatory metadata | LOCKED / RULED | [LAT]; S0 §13 |
| PERF-9 | Bridge has its own 30–60 min clock | RULED (R1) | BRR |
| PERF-12 | No history-sized work or chain lock on trading/read paths; readers never wait | PROPOSED (accepted audit criteria) | RR §1; AU §2 |
| PERF-13 | Qt first painted book p95 ≤ 50 ms | Existing gate | [CC] |
| PERF-14 | Throughput | OPEN → PLAN-10 default | [HL] §2.1 |
| DUR-1 | Durable intent before signing; durable signed object before sending; durable decision before reporting | RULED direction | RP §3; [CC] |
| DUR-2 | Signer journals never deleted; no unlock; journal loss = key retirement | RULED 2026-09-05 | [m:b3-finality-signer-recovery] |
| DUR-3 | A restored store fences signing | PROPOSED → PLAN-8 | S0R |
| DUR-4 | Timeout = "outcome unknown"; exact-byte resend only; reconnect never re-signs | RULED direction | S0 §12; [CC] |
| DUR-7 | G5: exact resend of an included-but-not-executed action | OPEN → PLAN-1 default | [m:b3-hyperliquid-learnings] |
| DUR-8 | Storage error = explicit safe state, never an invented receipt | RULED direction | RP §3 |
| CONS-1 | PoS V1 frozen (60 s blocks, 333 B3 minimum, BIP340 block signatures, no slashing) | LOCKED | OD-1 |
| CONS-2 | M7 BLS finality; dual weight and headcount quorum | LOCKED | XF |
| CONS-3 | Lessons: self-vote deadlock at 811,631; full replay under `cs_main`; stuck producer set; headcount Sybil; 2 s dead-leader penalty | Facts | RR §1, §4 |
| CONS-6 | No timeout lowers quorum or clears signing history | RULED requirement | S0 §13 |
| CONS-7 | Four authorities: producers, finality signers, FN seats, Ethereum verifier | RULED framing | S0 §0 |
| CONS-8 | PoS V2 improvements required; specifics OPEN | RULED requirement | S0 §8; [CC] C5 |
| CONS-10 | Synchronous invalidation of superseded authority; anchor ancestry verified | Accepted audit criteria | RP §3 |
| CONS-11 | FlowMesh halt never stops B3; B3 never waits on FlowMesh | RULED | V1P §1 |
| CONS-12 | Height activation; incomplete schedule fails closed; no mainnet override | LOCKED | AC §55 |
| SEC-1 | "No key or committee can move the vault" | LOCKED → **RE-RULE** (PLAN-2) | [m:b3-root-concept] Q6 |
| SEC-2 | No discretionary rollback or bailout | RULED | DEX O-9b |
| SEC-3 | Anti-DoS before crypto; bounded queues; v1 token buckets | RULED (v1) → **RE-RULE** budgets | V1P §10 |
| SEC-5 | Clients verify certificates and proofs; UI never trusts the API | RULED direction | [m:b3-ui-architecture-direction] |
| SEC-6 | Reproducible builds; threshold-signed update manifests | RULED | [m:b3-hive-update-system] |
| SEC-7 | No mainnet contact in development; no new dependency without approval | RULED | CL |
| SEC-8 | Bitcoin `Mutex` annotations everywhere; TSan/ASan in Linux CI | Accepted audit criteria | AU C8 |
| SEC-9 | Independent audits before activation; mixed versions refused | RULED requirement | S0 §13 |
| SEC-10 | Disclose bridge risks (2/3 forging, Tether freeze, stranding) | Disclosure required | BR |
| DEC-1 | "As decentralized as possible, not 100%" | RULED 2026-08-23 | [m:b3-decentralization-stance] |
| DEC-2 | Light client in, BLS finality out; no signer set in consensus for minting (OD-8) | RULED | OD-8; BRR R5 |
| DEC-4 | No broadcaster whitelist, no mandated co-location, no closed binaries | PROPOSED | [HL] §5 |
| DEC-5 | Every active seat in the committee | RULED (v1) | V1P §1 |
| BRG-1/2/3 | bUSD = Ethereum USDT 1:1; fresh Vault/Verifier V2; one Ethereum vault; USDT0 forwarders; no LP, no signer mint | RULED (R3, R5) | BRR |
| BRG-4/5 | Sync-committee light client in B3; BLS certificates verified on Ethereum via EIP-2537 | RULED | OD-8; XF |
| BRG-6/7 | 30–60 min flows; regtest first | RULED (R1, R4) | BRR |
| BRG-9 | Sponsored native-B3 fees (`SIGHASH_ALL\|ANYONECANPAY`) | RULED (R10) | BRR |
| BRG-13/14 | Caps 10k default; committee rule B-22 | PROPOSED / OPEN | BR |
| UX-1 | Names: B3 FlowMesh, B3 Hive, B3 | LOCKED 2026-08-25 | [m:b3-product-identity] |
| UX-2 | Qt is the node UI; engine in the node; API-first | RULED | [m:b3-ui-architecture-direction] |
| UX-3 | Web/mobile app only after v1 | RULED | same |
| UX-4 | Qt trades with the validator off | RULED requirement | S0 §0 |
| UX-5/7 | Responsive HL-style book; show time, not microblocks; request-state chips | RULED request / direction | [m:b3-hyperliquid-learnings]; RP §5 |
| UX-8 | No-gas onboarding | RULED goal | [SQ] §1.2 |
| UX-9 | Agent keys post-V1; design OPEN | RULED timing | [HL] R15 |
| GOV-1 | General governance mechanism | OPEN (OD-3) | OD-3 |
| GOV-2 | Parameters change only at a versioned height | LOCKED | AC §55 |
| CMP-1 | Magic, port, `b3coin-*` compatibility IDs preserved | LOCKED | CL |
| CMP-3/4 | V1 bytes keep the V1 verifier; unknown versions fail closed; mixed versions refused | RULED requirement | S0 §10, §13 |
| MIG-1/2 | V1 markets close at P; all V1 liabilities survive | RULED | [SQ] §1.2; S0 §10 |
| MIG-3 | Parallel V1 claim service plus opt-in transfer | PROPOSED (R-D6) | S0 §10 |
| MIG-4 | Never spend in both versions | RULED requirement | S0 §13 |
| MIG-6 | Regtest, then live 4-VPS, then shadow fork, then decision | RULED | [SQ] §1.2 |
| OPS-2 | A working DEX first; listing and governance after | RULED 2026-09-30 | [SQ] §1.7 |
| OPS-3/4 | Solve-and-report; Codex implements, Claude audits; never push; no AI attribution | RULED | [m:b3-solve-and-report-rule]; CL |
| VIS-1/2 | Chain-of-chains is a new chain; B3 as spoke via `message_root` | RULED / PROPOSED | [m:b3-original-vision-chain-of-chains] |
| IMPL-1 | Reuse proven Bitcoin components; do not rewrite crypto or validation | RULED 2026-10-15/16 | [CC] |
| IMPL-2 | C++20 and Qt Widgets; no Rust initially | RULED 2026-10-16 | RP §2 |
| IMPL-3 | Build alongside in `src/flowmesh2/`; delete nothing | RULED 2026-10-16 | [CC] |
| IMPL-4 | blst v0.3.17 approved for BLS | RULED 2026-08-23 | [m:b3-bridge-bls-direction] |

### 2.2 New requirements this plan proposes

| ID | Requirement (PROPOSED) | Why | Critique |
|---|---|---|---|
| PLAN-1 | Nonce window of 100 per account, mandatory expiry by slot height, cancel by OrderId, provable `EXPIRED` outcome. **RE-RULE** of TRD-18. | A withheld action must never freeze an account | Security B2 |
| PLAN-2 | Disclose and re-rule SEC-1: "2/3 of registered seats can move value up to the limits" | Truthful custody statement | Security B1 |
| PLAN-3 | B3 checks per-asset conservation on every `CheckpointV2` (Merkle-sum liability totals) | Theft can only redistribute, visibly | Security B1 |
| PLAN-4 | Large withdrawals wait 360 blocks (≈6 h); a bounded `PauseV2` record can extend to 4,320 blocks (72 h) | Time to detect and react | Security B1 |
| PLAN-5 | Seat floor `Q_min` (withdrawals pause below it); seat maturity 1,440 blocks; at most max(4, ⌊k/4⌋) seats activate per epoch | Raise the cost and visibility of takeover | Security B1 [inf] |
| PLAN-6 | 24 h withdrawal limit ≤ the published cost of acquiring 2/3 of seats; fixed caps set at versioned heights | Loss bounded by attack cost | Security B1, S3 |
| PLAN-7 | FOCIL-style inclusion lists in votes; leader terms ≤ 8 slots until the MEV analysis; within-slot order by an unpredictable seed | Limit leader ordering power | Security S1 |
| PLAN-8 | Every restart collects signed high-water marks from 2f+1 seats; journal gap or CRC break = key retirement | Detect silent VM rollbacks | Security S6 |
| PLAN-9 | Admission rejects accounts with no committed state before crypto; budgets scale with balance or fees paid | Sybil-proof admission | Security S7 |
| PLAN-10 | Sentries fan out each action to all seats; proposals use 6-byte short IDs; byte cap per slot; throughput target 5,000 actions/s sustained, 20,000/s for 1 s bursts | Fit the leader's upload link | Performance P-3 |
| PLAN-11 | Authority epoch changes only on seat-set change or a reorg at or below the anchor; anchors carry their finality proof | A new B3 block must not stall trading | Performance P-2 |
| PLAN-12 | Gate profiles G-LOAD, G-BLOCK, G-FAULT, G-WAN, per-leader-term reporting, `nr_throttled = 0` | Test the real deployment | Performance P-1 |
| PLAN-13 | Futures only on underlyings with ≥ 3 independent external price sources; oracle = median over all ≥ 2f+1 vote-committed submissions | No self-referential oracle | Security S4; Feasibility F3 |
| PLAN-14 | Bridge certificate valid only if signers hold ≥ 50% of active stake; fixed release cap in bUSD at a versioned height | Bridge bounds that hold | Security S3 |
| PLAN-15 | At P, B3 pins the final V1 root; V1→V2 transfer is a B3 transaction that consumes the V1 nullifier | One nullifier space | Security S5 |
| PLAN-16 | Forced exit opens after 10,080 blocks (≈7 days) with no accepted checkpoint, counted by height; it redeems available + reserved + futures equity | Exit that a minority cannot block | Security S2 |

---

## 3. Architecture

### 3.1 Picture

```
   Traders: B3 Hive (Qt), CLI, bots          (web app only after V1, UX-3)
        |  HTTPS + SSE push, TLS 1.3, 0-RTT off; same signed bytes to 2 sentries
        v
 +---------------------------- SENTRY TIER (public, no whitelist) ----------------------+
 |  cheap checks before crypto -> BIP340 verify once -> fan out action to ALL seats     |
 |  serve verified views, streams, proofs; never a source of truth (SEC-5)              |
 +--------------------------------------------------------------------------------------+
        |  seat mesh: own port, BIP324 cipher + one BIP340 session signature, 4 lanes
        v
 +------------------------------- ONE SEAT OPERATOR -------------------------------------+
 |  b3d (Core 31.1 + frozen B3 rules)           |  libfm_core (FlowMesh V2, no cs_main) |
 |  - validation, cs_main ONLY here             |  - admission pool (N workers)         |
 |  - PoS V1 producers, M7 finality             |  - agreement thread (PBFT, 1)         |
 |  - VaultV2 + CheckpointV2 + PauseV2 rules    |  - STATE OWNER (1, sole mutator)      |
 |  - bridge light client, assets, FN seats     |  - commit log + signer journal        |
 |        |                         ^           |  - read views (RCU) -> API            |
 |        | finalized facts (IN)    | checkpoints as B3 txs (OUT)                       |
 |        +----------> CHAIN ADAPTER (epoch, cursor, proofs) <----------+              |
 +--------------------------------------------------------------------------------------+
        |  B3 P2P, port 5647, CConnman unchanged
        v
   Other B3 nodes  <->  relayers (anyone)  <->  Ethereum: Vault V2 + Verifier V2 (EIP-2537)
```

For DEX-1, `libfm_core` runs inside the `b3d` process. The boundary is still enforced at link level (`research-bitcoin-components.md` §5.1–5.2): `libfm_core.a` may not reference `cs_main`, `ChainstateManager`, `PeerManager` or `CConnman`. Whether to split into two daemons is decided at the DEX-1 review (§3.2).

### 3.2 Components and process/thread model

**Packaging decision (Feasibility F5 accepted).** DEX-1 uses one binary (`b3d` with `libfm_core` linked). This keeps UX-2 ("engine lives in the node") without a re-rule.

At the DEX-1 review, split into `b3d` and `flowmeshd` only if any of these holds:

1. A FlowMesh crash took down block production in the 72 h soak.
2. Memory or CPU interference broke a gate.
3. Release cadence needs differ.

The adapter has the same interface in both cases. A fatal FlowMesh storage error **fences FlowMesh only**: it stops signing and trading, keeps reads live, and leaves `b3d` running (CONS-11). Restarting FlowMesh then needs a process restart, scheduled by the operator.

| Thread | Count | Job | Bitcoin component |
|---|---|---|---|
| Mesh I/O | 1 per lane (critical, proposal, action, bulk) | Framing, BIP324 AEAD, bounded per-peer input | `BIP324Cipher`, ChaCha20-Poly1305 |
| API gateway | 1 event loop | HTTPS, SSE, admission handoff | OpenSSL (existing), univalue |
| Admission | N = min(cores − 4, `cpu.max` quota) | Caps → account-exists → nonce window → budget → BIP340 in batches ≤ 64 → cache | `serialize` caps, `CCheckQueue`, `CuckooCache`, secp256k1 |
| Agreement | 1 | PBFT state machine, timers, BLS aggregate/verify via a small pool | blst via `crypto/bls` |
| State owner | **1** | The only writer: apply slot, compute roots | `overflow.h`, `int128` |
| Hasher pool | ≤ 4 | Deterministic parallel subtree hashing | SHA-256 (SHA-NI/ARM) |
| Log writer | 1 | Group commit, one barrier per decision group | `FlatFileSeq`, strict `JournalCommit`, `DirectoryCommit` |
| Read replica | 1 | Applies certified slots from the log to a second copy; publishes immutable views and snapshots (Performance S-8) | `ReadSlot` pattern, LevelDB snapshots |
| Fact ingest | 1 | Consumes chain-adapter deltas | ZMQ-`sequence`-style cursor |
| Oracle lane (futures only) | 1 | Collects signed price submissions | — |

**Rules for Codex (all enforced in CI):**

- No lock is held across a disk, network or chain wait. Mark such functions `EXCLUSIVE_LOCKS_REQUIRED(!m_state)`.
- Use only Bitcoin `Mutex` members. No `std::mutex`, `RecursiveMutex` or `GlobalMutex` in new code.
- Readers never wait on the writer, disk or chain (PERF-12).
- Every queue is bounded. Overflow returns a definite refusal with a reason code.
- Cancel, status and health have reserved capacity (10% of each lane).
- No busy-polling. Size pools from cgroup `cpu.max`. Pin critical threads (agreement, state owner, log writer) to CPU sets separate from `b3d` validation (Performance S-5). On macOS, run them at user-interactive QoS (Performance M-5).

### 3.3 Chain adapter (the only seam)

**IN: `FinalizedFacts`.**

- **Contents:** deposits into `VaultV2` by outpoint; bUSD/rUSD mints to FlowMesh accounts; FN-v2 seat registrations and removals (with PoP); listing records; checkpoint acceptances; payout confirmations; exit nullifiers.
- **Delivery:** a monotonic cursor `(height, index)`, a snapshot followed by deltas, and resubscription on any gap (the Bitcoin ZMQ `sequence` pattern).
- **Anchor rule (TRD-4 kept):** a fact is usable once its block is M7-finalized **and** ≥ 30 blocks deep. A bare finalized pin is decision C-9, deferred to Tier 3.
- **Authentication:**
  - Each seat trusts its **own** fully validating `b3d`, as in Bitcoin.
  - Every proposal carries its anchor `(height, hash)` plus the M7 finality certificate reference and header path, so a lagging follower can verify the anchor or fetch it (Performance P-2).
  - Sentries and clients that run no `b3d` verify inclusion proofs (transaction + merkle branch + header chain to an M7-finalized checkpoint).
  - **No new `facts_root` consensus rule is needed** (Feasibility F1).
  - [verify in M7] Confirm that M7 certificates let a non-node verify the finality set lineage. If they do not, add a set-root commitment at P.
- **DEX-1 regtest mode:** "trusted facts", meaning depth-only with no finality requirement and the same operator. Code in this mode must refuse `MAIN`, `TESTNET` and `SIGNET` chain parameters at compile time **and** at start-up (Security M4).

**Authority epoch (PLAN-11, Performance P-2, keeps CONS-10).**

- `authority_epoch` changes **only** when:
  - (a) a seat-set fact is finalized, or
  - (b) `DisconnectTip` removes a block at or below the current anchor height.
- `ConnectTip` above the anchor does **not** change the epoch.
- The check is synchronous, inside `ConnectTip`/`DisconnectTip`, before any queued notification (CONS-10).
- A seat signs only if `facts.epoch == authority_epoch` and the anchor's ancestry is proven.
- A leader proposes a new anchor only after it has been locally final for ≥ 2 s. Followers accept any anchor ≥ their last certified anchor that they can verify.

**OUT: `CheckpointV2`** (§3.6 spec). Anyone may relay it to B3.

### 3.4 Consensus

#### 3.4.1 FlowMesh agreement (R-D1)

**Default: stable-leader PBFT ([CC] C4 profile).** Feasibility F4 is accepted. Simplex is not used unless model checking finds a PBFT view-change defect that cannot be repaired.

| Parameter | Spec |
|---|---|
| Phases | Pre-prepare (proposal) → prepare → commit. Execute-then-vote: the leader executes before proposing; followers re-execute before preparing. 3δ. |
| Quorum | ⌊2k/3⌋+1 seats. Never lowered (CONS-6). |
| Committee | All active, mature seats (DEC-5 kept). Designed and gated for k ≤ 128 all-to-all. The rule for k > 128 is a Tier 3 ruling, needed only if registrations approach 128. Two committee tiers are **not** built (Feasibility F6). |
| Leader | Term = 8 slots (PLAN-7) until the MEV analysis rules otherwise. Leader for term t = `active[H(epoch_seed ‖ t) mod k]`, skipping seats in the certified slow-set. This replaces FN-10 round-robin (**RE-RULE**, Tier 2). |
| View change | Timeout = max(floor, SRTT + 4·RTTVAR of slot completion). Floor = 50 ms co-located; on WAN, 3·δ_q + 4 × barrier p99.9. A term ends at its first view change (Performance S-4). The new leader re-proposes all pending actions (Performance S-7). |
| Slot | Event-driven. A heartbeat slot at least every 1 s. One slot in flight. Pipelining is deferred (Performance M-1). |
| Slot limits | ≤ 1,024 actions; proposal bytes ≤ `B_link × T_slot / (k−1)` (PLAN-10), e.g. ≈20 KB per 10 ms at 1 Gbit and k = 64; 4 KiB per action. |
| Proposal body | 6-byte salted SipHash short IDs (BIP152 pattern) for actions already fanned out by sentries; full bodies only for actions younger than 1·δ_q. Missing bodies are fetched from peers. |
| Inclusion lists | Each prepare vote carries ≤ 32 short IDs of valid actions the seat has held for > 2 slots without inclusion. Proposal s+1 must include every action listed by ≥ f+1 votes in slot s's certificate, unless the action is now invalid. A follower refuses to prepare otherwise (PLAN-7). |
| Certificate | BLS aggregate over `TaggedHash("B3/FM2/COMMIT", SlotHeaderV2)` plus a signer bitmap. Fills, fees and balance deltas are in `outcomes_root`, so **R2 = R1**. |
| BLS handling | Aggregate-verify once (≈1.24 ms at 43 signers). After one invalid share from a seat on an authenticated session, exclude that seat from optimistic aggregation for the epoch, and verify it individually in parallel only when its share is needed for quorum (Security M1, Performance S-3). |
| Fast path | 2δ "RULE-FV" path stays gated. Only if G-WAN fails and only after model checking. |
| Conflicts | Two conflicting certificates mean evidence plus halt. On restart, the branch extending the **last B3-accepted checkpoint** wins, and R1 receipts on the other branch are void. This is disclosed in the UI and docs (Security M8, SEC-2). |
| Halt | Fewer than quorum seats → safe halt. Reads stay live. Equivocation evidence is kept (SEC-11). |

#### 3.4.2 Base chain

- **Unchanged on the DEX path.** PoS V1 production and M7 finality stay as they are.
- **Month 1 fix (B0):** bounded, durable per-block undo for derived state (finality, stake, seats, vault, bridge), proven equal to full replay. This removes the 97 s replay under `cs_main` (CONS-3b). It is an implementation fix, not a consensus change.
- **Separate track at height C ([CC] C5, M10):**
  - Finality V2: lock only on an observed quorum; never finalize where you nullified; per-block finality.
  - PoS V2: fix the stuck producer set and the headcount Sybil.
  - Requires C-12 (CLAUDE.md rule 3) and D3/D4 rulings.
  - Bonds and slashing are **not** in the first release (Security M7, CONS-1). If later ruled, slashing burns via a policy output so a framer cannot profit.

### 3.5 Engine and matching

**Shape.** One deterministic writer (LMAX style) with fixed pools and dense indices.

**Book, per market side:**

- a level array over a 131,072-tick band (2^17);
- a two-level bitmap (2,048 + 32 words) for best-price search;
- a Fenwick tree for depth;
- intrusive FIFO queues in a fixed order pool;
- an order-ID table with fixed capacity and open addressing that is **never iterated**.

No floats, `std::unordered_*`, wall clock or pointer-order dependence (INV-9).

**Slot phases (spec):**

1. Consumed chain facts (deposits, seat changes, listings, checkpoint acceptances, payout confirmations, exit nullifiers).
2. Account actions (transfers, withdrawal requests → reserved).
3. Cancels, by OrderId.
4. Post-only placements. They are rejected with `WOULD_CROSS` if they cross.
5. Takers, matched under the active matching profile.
6. Resting of residual GTC quantity.
7. Futures sweep (M9 only).
8. Fees.
9. Conservation assert (per asset: Σ liabilities == vault-backed total), then roots.

**Order inside a phase:**

- Actions of one account run in ascending nonce.
- Across accounts, order is ascending `TaggedHash("B3/FM2/ORDER", parent_cert_hash ‖ ActionId)`. The seed is unknown when the client signs, so nobody can grind a better position (PLAN-7).

**Matching profile (R-D3; RE-RULE of D-1/TRD-7, Tier 1).**

- **Default A2:**
  - Makers rest in price-time FIFO.
  - All takers in a slot sweep the book together at maker prices.
  - At each level, taker quantity is allocated pro-rata; maker fills are FIFO.
- **Residual crossing:** after the sweep, same-slot GTC takers on opposite sides that still cross are matched in one max-volume uniform-price call before resting. The tie-break rule is part of the ruling, and the checker must cover it [inf].
- **Alternatives for the ruling:**
  - A: one uniform price per slot.
  - B: Hyperliquid-style one-by-one matching. This concedes ordering power to the proposer.
- **Display:** the client shows a normal HL-style book either way (UX-5).
- **MEV analysis** comparing A and A2 under leader slot-boundary attacks is an M3 exit criterion (Security S1).

**Caps (owner pins, Tier 1):**

| Cap | Value |
|---|---|
| Actions per slot | 1,024 |
| Fills per order | 1,024 (remainder → `FILL_CAP`) |
| Fills per slot | 16,384 |
| Changed state leaves per slot | **8,192** (Performance S-2); 32,768 only if deterministic parallel subtree hashing passes the 5 ms gate on the target x86 server CPU |
| Open orders per account | 1,000 |
| Spot price band | ±50% of the epoch reference price |
| Overflow | Definite reject: `BOOK_FULL`, `FILL_CAP`, `SLOT_FULL` |

**Money.**

- Balances are `int64` atoms with portable checked `int128` intermediates.
- Prices are integer ticks; quantities are integer lots. Tick and lot are set per market at listing (OD-7, Tier 1).
- Rounding always goes against the actor. The residual goes to a named account (default: treasury; dust rule D6 OPEN).

**Fees (C-2, Tier 1).** Default: the seller pays 100 ppm of matched notional once, in Q, rounded up. Split 80% to the seat pool (equal per active seat per epoch, C-3 v1 reading; claimable by the historical BLS key, FN-12) and 20% to the treasury account. Both incidences are implemented behind the profile switch.

**Engine gate.** Apply plus roots p99 ≤ 5 ms at the maximum batch on an adversarial 131k-tick book with ≥ 10k resting orders, on the target x86 CPU. The probes measured 57–90 µs for matching 1,024 orders; hashing dominates.

### 3.6 Accounts, assets, balances, custody

| Asset | On B3 (UTXO, Policy Outputs) | On FlowMesh (accounts) |
|---|---|---|
| B3 | Native; base fees; STAKE; AssetId 0 | Base asset of B3/Q |
| FN | Amount-1; PoD on the pinned curve; an FN-v2 output is a seat | Not deposited by default |
| bUSD / rUSD | `BRIDGE_BACKED`, light-client minted | Quote asset Q |
| Colored assets | `GENESIS_FIXED`; V2 AssetId hashes the rules document | Exact AssetId only |

**Account.** `account_id = TaggedHash("B3/FM2/ACCOUNT", xonly_pubkey)`. An account exists once a deposit fact creates its leaf.

**Action wire format (spec, PLAN-1):**

```
ActionV2 (Bitcoin serialize; every CompactSize capped):
  u8      version            = 1
  u256    domain             = TaggedHash("B3/FM2/DOMAIN", ModernChainDomain || fm_version || net_magic)
  xonly   signer             (32 B)
  u64     nonce              (window rule below)
  u64     ref_slot           (a certified slot height the client has seen)
  u16     max_delay          (<= MAX_DELAY_SLOTS = 1,024)
  u8      kind               (append-only enum: PLACE, CANCEL, CANCEL_ALL, WITHDRAW, TRANSFER, ...)
  bytes   payload            (<= 4 KiB, kind-specific)
  sig64   bip340_sig         over msg = all fields above
ActionId = TaggedHash("B3/FM2/ACTION", msg)        (signature excluded)
OrderId  = ActionId of the PLACE
```

`fm_version` and the network magic are inside `domain` (Security M3). An action is therefore invalid on any other network or FlowMesh version.

**Nonce window (replaces the strict sequence; HL pattern, `hyperliquid-learnings-report.md:120`).**

- State keeps `used`: the up to 100 highest nonces used by the account.
- Accept nonce n iff `n ∉ used` and (`|used| < 100` or `n > min(used)`).
- On accept, insert n and evict the minimum if the set exceeds 100.
- A different ActionId with a used nonce gets a definite `NONCE_USED`.
- A different ActionId with the same nonce as one still in the pool gets `NONCE_PENDING`; the first admitted wins.

**Expiry.** An action is valid in slot s only if `ref_slot < s ≤ ref_slot + max_delay`.

- After that window, the client proves a definite **`EXPIRED`** outcome from any later certified account leaf. The nonce is absent from `used`, and the window has passed, so it can never be included.
- Heartbeat slots (≥ 1 per second) bound the worst case to about 17 minutes. Under load the window is seconds.
- A time-based "good-til-time" may be added later using a median-of-seat-timestamps rule (BIP113 pattern), if TRD-19 is ruled that way.

**G5 default.** Exact-byte resend is allowed and safe. A duplicate returns the original outcome, and after expiry it returns `EXPIRED`.

**Cancels** address `OrderId`, never a sequence, so a stuck action never blocks a cancel (Security B2).

**Balances.**

- Spot: `(available, reserved)` per `(account, AssetId)`, with atomic cross-market reservations (TRD-10).
- Futures: a separate domain, entered only by explicit transfers (TRD-13).
- Each account leaf also stores `exit_value[asset]` = available + reserved + futures equity at mark, floored at 0. It is updated at checkpoints and used by the Merkle-sum tree and the forced exit.

**Custody (keyless, B3-checked).** `VaultV2` is a set of B3 policy outputs with no key. Deposits are credited once per outpoint after the anchor rule. Withdrawals become **effects** in a checkpoint and are redeemed by receipt proof plus a nullifier.

**`CheckpointV2` (MPA record; number assigned from the single registry, C-20).** It carries:

```
domain, epoch, seq, parent_checkpoint_hash, last_slot_header_hash,
state_root, effects_root, liability_sum[asset] (Merkle-sum roots),
facts_cursor, seat_set_root, cert (BLS aggregate + bitmap over the seat set at facts_cursor)
```

B3 consensus checks the following at P (PLAN-3/4/5/6/15/16):

1. `parent` is the last accepted checkpoint, `seq = parent.seq + 1`, and at most one checkpoint per block.
2. The certificate is signed by ≥ ⌊2k/3⌋+1 of the **mature** seats at `facts_cursor`.
3. **Conservation:** for each asset a, `Σ liabilities_k[a] == D(facts_cursor)[a] − E_k[a]`.
   - D = cumulative deposits consumed.
   - E = cumulative effects issued (withdrawals and fee payouts).
   - B3 already knows both.
   - Clients check that every sibling sum on their own path is in [0, 2^62].
4. Effects are redeemable only if k ≥ `Q_min`, within the per-checkpoint and rolling 24 h (1,440-block) caps.
   - Effects ≥ the large threshold (default 50,000 bUSD-equivalent per asset, owner pin) wait 360 blocks.
5. `PauseV2`: either ≥ f+1 seats of that seat set, or finality signers holding ≥ 1/3 of finality weight, may sign a pause.
   - The pause extends the wait of unredeemed large effects to 4,320 blocks (72 h) from the pause.
   - At most 4,320 paused blocks per 10,080-block window. A pause cannot block deposits or small withdrawals.
   - Pause power delays; it never moves funds or vetoes (INV-8 bounded).
6. Forced-exit rules (§6.5): if forced exit has been used, a later checkpoint must cover all exit nullifiers through `facts_cursor`.

**What custody means now (PLAN-2, RE-RULE of SEC-1).** "No single key can move the vault. Two-thirds of registered FlowMesh seats acting together can redistribute balances and withdraw up to the published limits. B3 refuses any root that creates value beyond the vault. Your wallet checks your own balance against every accepted checkpoint and warns you."

What happens after a detected false root that survives the pause window is OPEN (Tier 3). Honest seats can halt the domain, which leads to the forced exit against the last accepted root. A design that excludes disputed roots from the exit base touches SEC-2 and needs a ruling.

### 3.7 Storage and commitments

**Commit log (critical path).**

- Segment files `fm2/log/NNNNNNNN.seg`, 64 MiB each. They are **zero-filled** at creation, then flushed and directory-committed (Performance S-9).
- Record format: `[u32 len][u32 crc32c][u8 type][payload][32 B chain = SHA256(prev_chain ‖ record)]`.
- Types: `INTENT`, `SIGNED_VOTE`, `DECISION` (header + certificate), `APPLIED` (slot, state_root), `SNAPSHOT_MARK`.
- Group commit: one barrier per decision group, never under the state-owner lock.
- `JournalCommit` = `fdatasync` on Linux, `F_FULLFSYNC` on macOS.
  - **Any** error, including `EINVAL`, sets a sticky fatal flag, stops signing and fences FlowMesh.
  - It never retries an fsync. Bitcoin's `FileCommit` ignores `EINVAL`; ours must not (DUR-8).

**Signer journal.**

- Separate segment set. Never deleted (DUR-2). Records `(epoch, slot, phase, digest, signature)`.
- **Strict mode (default, DUR-1):** intent barrier, then sign, then signed-vote barrier, then send.
- **Folded mode (Tier 2 option):** sign deterministically (BLS), write the signed vote with one barrier, then send. This saves 3 barriers per slot (≈16 ms on the Mac). It is safe because nothing unpublished can equivocate.

**Restart (PLAN-8).**

- On **every** start, a seat asks peers for signed high-water statements: the highest slot in which each peer saw this seat vote.
- It resumes only after 2f+1 answers, and only above max(local, reported) + 1.
- If a reported vote is newer than the local journal, or the journal has a gap or a CRC break, the key is retired (DUR-2/3).
- Below 2f+1 reachable peers, the seat stays read-only.

**State.**

- State is held in memory. The read replica takes LevelDB snapshots at each checkpoint, on a device other than the log device when one exists.
- Recovery = latest snapshot + exactly-once replay of `APPLIED` records, idempotent by slot number. Estimated ≈5 s at 1M leaves [inf].
- LevelDB never sits on the per-action path (it stalls writes at 8 or more level-0 files).

**Commitments.**

- Tagged SHA-256 binary sparse Merkle trees, one per component: balances, reservations, orders, positions, markets, nonce windows, seat accruals, withdrawals, fact nullifiers.
- A **Merkle-sum** SMT per asset for liabilities.
- All roots sit under one root-of-roots.
- RFC 6962 trees cover each slot's actions and outcomes, plus a history tree over slot headers.
- A balance proof is ≈768 B. 1,000 updates take ≈2.2 ms on Apple M-series hardware; the gate runs on the x86 target.

**Retention.** Journals, evidence and V1 claims are kept forever. Activity-driven slots grow journals by ≈6 GB/yr [inf]. Archive seats serve full state at each checkpoint (FN-11). Clients persist their own leaf proof for every accepted checkpoint (Security S2).

**Hardware.** Qualify on Linux NVMe with power-loss protection, and record a start-up barrier probe in every run's metadata. PLP is **recommended and disclosed, not mandatory** for community seats (Feasibility M2, DEC-1). A separate log device is recommended, not required (Feasibility M4).

**Client outbox.** SQLite in WAL mode with `synchronous=FULL` and `fullfsync`. Bursts are group-committed (Performance M-3).

### 3.8 Network and client API

**B3 P2P.** `CConnman` on port 5647, unchanged.

**Seat mesh.**

- Own port (C-10, **RE-RULE**, Tier 2; default 5649, firewalled to seats and sentries).
- One TCP connection per lane: critical, proposal, action, bulk.
- BIP324 cipher plus one BIP340 signature over the session ID. This needs crypto review; the fallback is the BOLT 8 Noise handshake. No per-frame signatures.
- Seat endpoint records are signed by the seat key and gossiped **only inside authenticated seat sessions**, never written in plaintext on chain. Each seat sits behind sentries (Security S8).
- Budgets are derived from protocol rates (slot rate × k × message types), replacing the v1 8 msg/s bucket (SEC-3 **RE-RULE**, Tier 2).
- Linux settings in the `seat` role preset (operator-applied, documented): per-route `rto_min ≈ 5 ms` and `tcp_slow_start_after_idle=0` on mesh routes (Performance S-6).

**Public API (sentries).** HTTPS plus **Server-Sent Events** over the existing OpenSSL stack. No new WebSocket dependency (Feasibility M3, SEC-7). OpenSSL ≥ 3.5, TLS 1.3, 0-RTT off.

| Endpoint | Behaviour |
|---|---|
| `POST /v2/actions` | Body = exact `ActionV2` bytes. Idempotent by ActionId. Immediate definite answer: `admitted`, `duplicate` (with the original outcome reference), or `refused` with a code (`NONCE_USED`, `NONCE_PENDING`, `EXPIRED`, `NO_ACCOUNT`, `BUDGET`, `SLOT_FULL`, `BAD_SIG`, …). |
| `GET /v2/actions/{id}` | `pending`, `included` (slot, outcome, certificate, Merkle paths), `expired` (with proof) or `unknown`. |
| `GET /v2/stream` (SSE) | Channels for account, book, trades and slots. Every message carries a `(stream_epoch, seq, head_slot)` envelope. Snapshot, then deltas; 1 s heartbeat; resubscribe on any gap. |
| `GET /v2/state/{account}` | Leaf plus proof against a certified `state_root`, labelled `certified` or `checkpointed`. |

**Admission order (PLAN-9).**

1. Frame and size caps.
2. Kind known.
3. Account leaf exists (otherwise `NO_ACCOUNT`).
4. Nonce not already excluded by the window.
5. `ref_slot` not expired.
6. Budget available.
7. **Then** BIP340 verification, in batches of ≤ 64.
8. Dedup and conflict decisions only after the signature verifies.

Budget default (SEC-4 OPEN; PROPOSED, HL-like): 10,000 initial credits per account, plus 1 credit per 1 Q of traded notional. Cancels always have the reserved 10% lane, up to the account's open-order count.

**Client rules.**

- Durably save the exact bytes, then post them to **two** sentries: immediately, or as a hedge at the p90 delay (Security M6, Performance S-6).
- Prewarm the TLS connection when the order ticket opens.
- A timeout means "outcome unknown". Never re-sign (DUR-4).

### 3.9 Bridge

The mainnet track finishes Bridge V2 under R3/R5. It does not redesign it.

- **Shape.**
  - One keyless Ethereum Vault V2 is the only reserve.
  - Verifier V2 starts from a fresh `START_CERTIFICATE` on the **existing M7 gadget** under a new domain. That avoids waiting for C (Feasibility F1).
  - Arbitrum users enter through USDT0 forwarders. No LP, no Arbitrum vault, no signer minting (BRG-3).
- **Inbound:** the sync-committee light client in `b3d` mints bUSD straight into a `VaultV2` deposit for a FlowMesh account.
- **Outbound:** a withdrawal effect becomes a packet in a B3 `message_root` (the generalized `withdrawal_root`, VIS-2). The finality certificate is verified on Ethereum via EIP-2537. Anyone relays.
- **Hardening before any mainnet deployment:**
  1. Safe exporter (P3): sign only after a confirmed 2/3 commit.
  2. **Validity floor (PLAN-14):** a certificate is bridge-valid only if its signers hold ≥ 50% of total active stake, not just of opted-in stake (Security S3).
  3. **Fixed caps in bUSD at versioned heights:** pilot 10,000/day, then ≤ 250,000/day. Transfers ≥ 50,000 wait 2 h. Mints ≤ 10,000 per block and per epoch (BRG-13).
  4. Ethereum fork table; Gloas support shipped ≥ 30 days before Glamsterdam; unknown fork → mints halt.
  5. A USDT0 DVN monitor.
- **Deferred:** bonds and slashing (C track). Auto-pausing on a bond/limit ratio is also deferred, because it needs a non-self-referential B3 price inside consensus. Instead, the fixed cap is set against the bond at a stress price of 30% of current [inf].
- **Flow times [inf]:**
  - Deposit ≈45–55 min under the 30-block anchor, or ≈15–25 min with a finalized pin (C-9).
  - Withdrawal ≈20–35 min: checkpoint cadence (default every 10 B3 blocks or on demand) + M7 finality (12–21 min) + Ethereum inclusion.
  - Route d3 (43–63 min) can exceed R1's 60 min. Disclose.
- **Disclose (SEC-10):** a 2/3 finality coalition is bounded only by the caps until ZK proofs. The sync committee is unslashable, so mint limits are not value at risk. A coalition can decide whether its own evidence is finalized. Tether can freeze the reserve.

C-6 (CLAUDE.md rules 5–6 versus R3/R5) must be reconciled before mainnet bridge work (Tier 3).

### 3.10 Governance and fees

- **Listing (after DEX-1, AST-8):** a B3 transaction pays 100,000 B3 to the treasury and carries a listing MPA record (number from the single registry, C-20). The market opens after ≈1,440 blocks of notice (PROPOSED Q37–Q57).
- **Removal:** by 2/3+1 of mature seats, as cancel-only then claim-only. Never a seizure (AST-6).
- **Fees:** issuance 1,000 B3; trading 100 ppm in Q, split 80/20 (§3.5). The base chain is unchanged: native-B3 fees, the 90/10 subsidy and the single-key treasury.
  - The treasury now also receives the 20% fee share and listing fees. Re-consider multisig (Security M5; optional ruling; ECO-3 stands until ruled).
- **Parameter changes** happen only at versioned heights (GOV-2). The general mechanism stays OPEN (OD-3).

### 3.11 Futures

Only "futures, max 10×" is LOCKED. TRD-16 forbids starting with funding, ADL or cross margin.

- **First product (PLAN-13):** dated, cash-settled, isolated, linear futures with collateral in Q.
  - They expire at a certified slot height and settle to the oracle median over the last N oracle rounds.
  - Perpetuals come later, by ruling.
- **Underlyings:** BTC/Q and ETH/Q, which have deep external markets. **No B3/Q futures until external liquidity exists** (Security S4).
  - The owner rules on the underlying and the named sources before M9 design starts (Feasibility F3).
- **Oracle:**
  - Every 3 s, each seat submits a signed price: the median of ≥ 3 named external venues, fetched by an operator-run sidecar outside the node. That keeps new dependencies out of `b3d` (SEC-7), but the sidecar needs approval.
  - The certified oracle is the median over **all ≥ 2f+1 submissions committed in prepare votes**. The leader cannot choose which ones count.
  - If the oracle is older than 10 s, risk-increasing actions get `ORACLE_STALE`.
- **Margin:** initial = 1/leverage; maintenance = 50% of initial.
- **Mark:** median of (oracle, book mid, oracle-anchored impact price). Never the last trade.
- **Liquidation:**
  - A liquidation-price index replaces scanning every position (Performance S-8).
  - Liquidations go through the book with a per-slot liquidation cap, then the insurance reserve, then a declared deficit halt (cancel-only, settle at the last good oracle).
  - No ADL, no house vault.
  - Open interest capped per market at a fixed Q amount and at ≤ 10× the insurance reserve [inf].
- **Release shape (C-5/TRD-14, Tier 3):** spot activates at P. Futures activate at P_f, scheduled in the same release binary. The owner rules whether this satisfies "never a spot-only V2".

### 3.12 Clients

- **`b3verify`:** one C++ library for certificate and Merkle-proof checks. Qt, the CLI, tests and the independent checker all use it. Clients check sum-tree path bounds on their own leaf at every accepted checkpoint and warn on any unexplained decrease.
- **B3 Hive (Qt 6 Widgets).**
  - Wallet and stake pages use `b3d`. The trade page uses the FlowMesh API only, never RPC (C-11).
  - Push-fed models, time not slots, request-state chips (UX-5/7).
  - Labels distinguish `certified` and `checkpointed` (INV-13).
  - One connection indicator.
  - Gates: R3 p95 ≤ 30 ms; first painted book p95 ≤ 50 ms.
  - Engine-off on Windows and macOS (UX-4).
- **Web:** only after V1 (UX-3).

---

## 4. Reused Bitcoin components

Upstream paths are in Bitcoin Core 31.1 as carried by the B3 tree. Modes:

- **LINK**: linked unchanged.
- **COPY**: copied into `libfm_core` with the noted change.
- **PATTERN**: the design is reused, not the code.
- **NODE**: stays inside `b3d` only.
- **NO**: not used.

| Component | Upstream path | Mode | Why |
|---|---|---|---|
| libsecp256k1 (BIP340, x-only, ellswift) | `src/secp256k1/` | LINK (0.8.0 upgrade as its own reviewed commit) | Constant-time, decade-hardened signatures for actions and sessions |
| Key wrappers | `src/key.cpp`, `src/pubkey.cpp`, `src/musig.cpp` | LINK | BIP340 signing with aux randomness |
| SHA-256 (SHA-NI/ARM dispatch), `HashWriter`, TaggedHash | `src/crypto/sha256*.cpp`, `src/hash.h` | LINK (BLS moved to its own `b3_bls` target) | Fast, self-tested hashing; domain separation |
| SipHash + BIP152 short IDs | `src/crypto/siphash.*`, `src/blockencodings.*` | PATTERN | 6-byte salted IDs keep proposals within the leader's link (PLAN-10) |
| ChaCha20-Poly1305 + BIP324 cipher | `src/bip324.*`, `src/crypto/chacha20poly1305.*` | COPY (cipher only; `V2Transport` is tied to `CNode`) | Encrypted seat mesh; add one session signature |
| CRC32C | `src/crc32c/` | LINK | Log-record checksums |
| `serialize.h`, `streams.h`, `span.h` | same | COPY + per-type size caps | Canonical bounded encoding of every V2 object |
| `uint256` | `src/uint256.h` | COPY | Identifiers |
| Amount, overflow, int128 | `src/consensus/amount.h`, `src/util/overflow.h`, `src/util/int128.h` | COPY (MSVC fallback checked by vectors) | Exact money arithmetic |
| `sync.h`, `threadsafety.h` | same | LINK, mandatory | The compiler rejects lock mistakes; zero release cost |
| `CCheckQueue` | `src/checkqueue.h` | COPY | Parallel BIP340 admission |
| `CuckooCache`, signature cache | `src/cuckoocache.h`, `src/script/sigcache.cpp` | COPY / PATTERN | Verify each action once |
| `FlatFileSeq`, `FileCommit`, `DirectoryCommit`, `AllocateFileRange` | `src/flatfile.*`, `src/util/fs_helpers.cpp` | COPY + a stricter `JournalCommit` (`EINVAL` fatal, no retry, zero-filled segments) | Append-only crash-safe log |
| `dbwrapper` + LevelDB 1.22 | `src/dbwrapper.*`, `src/leveldb/` | LINK (indexes and snapshots only) | Proven key-value store; kept off the hot path |
| `util/*` (fs, time, thread, logging, random) | `src/util/` (`bitcoin_util`) | LINK | Mature plumbing |
| ZMQ `sequence` topic | `src/zmq/` | PATTERN | Cursor and gap detection for facts and streams |
| Median-time-past | `src/chain.h` (`GetMedianTimePast`) | PATTERN (only if TRD-19 rules time-based expiry) | Deterministic time from signed timestamps |
| univalue | `src/univalue/` | LINK | JSON for the API |
| `CConnman`, addrman, banman, asmap | `src/net.cpp`, `src/addrman.cpp`, `src/banman.cpp` | NODE | Base P2P with proven eclipse resistance |
| Validation, headers sync, mempool, block storage, `net_processing` | `src/validation.cpp` etc. | NODE (frozen) | History validity; the only `cs_main` |
| B3 legacy and era-1 rules | `src/legacy/*`, `src/modern/*.h` | NODE; header-only `b3_modern_rules` for proofs | History to X and from 811,001 |
| Wallet: descriptors, BIP32, PSBT, SQLite | `src/wallet/*`, `src/script/descriptor.cpp`, `src/psbt.cpp` | NODE / LIB in Hive | Custody; sponsored-fee PSBTs (R10) |
| HTTP server (libevent) | `src/httpserver.cpp` | NODE (operator RPC only) | No TLS; not for the public API |
| Fuzz, functional tests, sanitizers, lint, nanobench, Guix | `src/test/fuzz/`, `test/functional/`, `ci/`, `test/lint/`, `contrib/guix/` | COPY / LIB | The cheapest proven assurance |
| libbitcoinkernel | `src/kernel/` | NO (unstable, contains `cs_main`) | Revisit when released |
| libmultiprocess / Cap'n Proto | `src/ipc/` | NO (no Chain schema, no Windows) | Revisit if a split is ruled |
| minisketch / Erlay | `src/minisketch/` | NO | Bandwidth tool, not latency |

**B3's own reused pieces:**

- blst v0.3.17 behind `crypto/bls`, as its own target;
- the Ethereum verifiers in `src/bridge/` (extract header-only `b3_eth_verify` and cut the `node/validator_set.h` include);
- `contracts/src/v2/` (2,739 lines, not yet externally audited);
- the finality signer store discipline (never delete; magic and version);
- Python models as vectors and reference oracles.

**Mechanical boundary (CI):**

1. `nm --undefined-only libfm_core.a | c++filt` must show no `cs_main`, `ChainstateManager`, `Chainstate::`, `PeerManager` or `CConnman`.
2. A forbidden-include lint (`validation.h`, `kernel/cs_main.h`, `net_processing.h`, `node/*`, `txmempool.h`, `index/*`, `interfaces/chain.h`).
3. Clang `-Wthread-safety -Werror=thread-safety`.
4. A TSan job with an **empty** suppression list. Do not inherit upstream's `deadlock:Chainstate::ConnectTip`.

---

## 5. Language and toolchain decision

| Choice | Decision | Reason |
|---|---|---|
| Language | **C++20** | RULED (IMPL-2). It links Bitcoin's proven code without a foreign-function boundary. Rust is rejected for the first redesign by ruling. Protocol references written in Rust (Commonware Simplex) are read, not linked. |
| Build | CMake; Clang ≥ 18 and GCC; Guix for releases, matched by 2 builders | Thread-safety analysis needs Clang; Guix gives reproducible binaries (SEC-6) |
| Windows | mingw-w64 GCC via Guix (native `__int128`); MSVC fallback compared on shared vectors | Engine-off Hive on Windows (UX-14) |
| UI | Qt 6 Widgets | RULED; QML only if a measured benefit exists |
| Models | Python 3: reference models produce vectors and an independent checker | Already exists (`test/flowmesh_v2_*`) |
| Contracts | Foundry, solc 0.8.35 | Existing |
| Lint bans in `src/flowmesh2/` state code | `float`, `double`, `std::unordered_*` iteration, wall-clock calls, `std::mutex` | INV-9, SEC-8 |
| Dev-only tools (approval needed, Tier 1) | Quint + Apalache (JVM/Node), LazyFS (Linux FUSE), AFL++ | Model checking, fsync fault injection, fuzzing; none ship in the node |
| CI hardware (approval needed, Tier 1) | 1 Linux x86_64 box, ≥ 32 cores, PLP NVMe; plus macOS arm64 and Windows x64 runners | ≈256 CPU-h per night of DST [inf]; target-CPU gates |

---

## 6. Continuity with B3

### 6.1 What is preserved

- **Nothing in history changes:** genesis, every historical txid, outpoint, script and value; legacy validation to X (trusted replay, U == U′ passed at H); the corridor; FN Genesis (3,592 outputs at 810,001); S_H, R0, the halving and the 90/10 split already in coinbases; every modern block from 811,001 validating under its original rules.
- **Live state:** STAKE outputs, M7 finality pins and certificates, FN v1/v2 outputs and seat bindings, issued assets, the treasury and its funds.
- **Obligations:**
  - Signer journals are never erased.
  - FlowMesh V1 liabilities survive. Mainnet seats were not live as of 2026-10-03, so they may be nil; the owner verifies this read-only on his own node.
  - Regtest tester datadirs are untouched.
  - Accepted Ethereum roots stay claimable. Disclose the old vault's 3 test USDT. Legacy bUSD stays frozen.
- **Identity:** magic, port, `b3coin-*` IDs, `b3coin:` URI, product names.

### 6.2 Activation heights

| Height | Activates | Consensus scope |
|---|---|---|
| **P** (FlowMesh V2) | `VaultV2`, `CheckpointV2`, `PauseV2`, conservation check, withdrawal caps and delays, `Q_min`, seat maturity and pacing, forced-exit rules, V1 close and V1 root pin, opt-in fork-id, wire-id bump | Required for any mainnet V2 DEX |
| **P_f** (futures) | Futures markets and the oracle lane | Same binary as P, later height (Tier 3 ruling) |
| **C** (base chain, separate track) | Finality V2, PoS V2 fixes, optionally `facts_root` | Not on the DEX path |

### 6.3 Replay protection

1. **Fork-id (opt-in, Security M2).** A new sighash domain commits `ForkId = TaggedHash("B3/FORK", ModernChainDomain ‖ P ‖ hash(P−1))`, modelled on SIGHASH_FORKID. Wallets use it by default after P. Old transaction forms stay valid forever (INV-4/14), including presigned and `LEGACY_LOCK` spends. Making it mandatory would need a separate ruling.
2. **Domain tags.** Every new signed object carries the version and `ModernChainDomain`: actions, seat bindings, certificates, checkpoints, receipts.
3. **Bridge.** Contracts read the chain id at run time and bind the verifier address (R9).
4. **Wire.** The modern wire id is bumped at P, as 80009 → 80010 was for the 811,631 recovery. Mixed versions are refused (CMP-4).
5. **Sunset.** A release well before P carries a hard-coded sunset height. After it, old binaries refuse to stake or sign and show a clear message. Current v1.1.3 binaries have no sunset, so observer mode needs ≥ 90% of stake on the new release before P is set [inf].

### 6.4 V1 FlowMesh cutover (MIG-1..4, PLAN-15)

- **At P:**
  - V1 markets close, and resting orders are released.
  - B3 **pins the final V1 checkpoint root** (the empty root if V1 never checkpointed on mainnet).
  - V1 checkpoints after P are rejected, so live V1 committee keys cannot mint new V1 receipts (Security S5).
- **Claims:** a parallel V1 claim service redeems by proof against the pinned root, with no deadline and no snapshot credit.
- **Transfer:** an opt-in V1→V2 transfer is a **B3 transaction** that consumes the V1 claim nullifier and creates a `VaultV2` deposit outpoint. The V2 credit then arrives as an ordinary deposit fact. That gives one consensus-enforced nullifier space.
- **Liability manifest:** derived twice, byte-equal, publicly challenged and signed before P.
- **Journals:** V1 signer journals are imported read-only. The first V2 certificate chains to the last V1 one (DUR-2).

**Double-spend controls per value bucket** (`research-migration.md` §8): native B3 (sunset, fork-id), STAKE (journals, sunset), FN seats (historical-key rewards, new V2 domain), assets (ordinary outputs), bUSD (disjoint lineages, R9 binding), V1 balances (pinned root plus nullifier), V1 orders (closed at P), client outbox (no automatic re-sign across versions).

### 6.5 Forced exit (PLAN-16; the rule needs a Tier 3 ruling)

- **Trigger:** `tip_height − height(last accepted CheckpointV2) ≥ 10,080` (≈7 days), evaluated in block validation. It counts height, not finalized blocks, so a finality minority cannot block it (Security S2.5). Seven days is longer than the 811,631 recovery took (S2.1).
- **Redeem:** `exit_value[asset]` from the last accepted root (available + reserved + futures equity at that root's mark). Each account's nullifier is single-use, and exits have their own rate limit.
- **Data:** clients keep their own proofs; archive seats publish full state at each checkpoint (S2.3).
- **Resume:** a later checkpoint must cover every exit nullifier through its `facts_cursor`, so an exited balance cannot be spent again (S2.4).

### 6.6 Proof that history did not drift

Extended U == U′: a commitment over the UTXO set and every tracker (stake, finality, FN, assets, vault, bridge) must be byte-equal between the current release and the new `b3d` at H, M, A1–A3, every 10,000 blocks and the tip. This runs on a shadow fork with isolated magic and ports, never against mainnet (SEC-7). It is required for every `b3d` consensus change (P, then C).

### 6.7 Rehearsal (MIG-6)

1. Regtest with configurable P.
2. The live 4-VPS regtest session plus the Mac.
3. A real-history shadow fork.
4. ≥ 4 weeks of observer mode.
5. The owner's decision, with ≥ 90% of stake upgraded [inf].

Path B (a hub with its own genesis) stays possible later through the same adapter. Freeze-and-claim is rejected (INV-1/3/14).

---

## 7. Latency budget and measurement

### 7.1 Budget per stage (R1 = R2, one order, strict 9 barriers, ms) [inf]

Assumptions:

- Barrier 0.2 ms on Linux PLP and 5.5 ms on the Mac.
- BLS sign 0.18 ms; aggregate verify 1.24 ms at quorum 43.
- Follower re-execution included.
- The waiting time is half a slot, and a slot is the sum of its own stages.
- These are **models**, not targets. [CC] forbids inferring WAN results by adding delay to a local median, so G-WAN measures them.

| Stage | Co-located idle (Linux PLP, k=64) | Co-located max batch (x86, 1,024 actions) | Regional US+EU core (δ_q≈40) | Global 64 seats (δ_q≈95) | Owner's Mac, 1 host (k=4) |
|---|---|---|---|---|---|
| Sign + outbox barrier | 0.1 | 0.1 | 0.1 | 0.1 | 5.6 |
| Client → sentry | 0.25 | 0.25 | 0.25 | 0.25 | 0.1 |
| Admission (caps, nonce, BIP340) | 0.1 | 0.5 | 0.1 | 0.1 | 0.1 |
| Sentry → all seats | 0.3 | 0.3 | 35 | 70 | 0.1 |
| Wait for the next slot (half a slot) | 4.0 | 8.0 | 63 | 146 | 21.9 |
| Leader execute + roots | 1.0 | 3.5 | 1.0 | 1.0 | 1.0 |
| Proposal (2 barriers, sign, δ_q, bytes) | 1.1 | 4.2 | 40.6 | 95.6 | 11.3 |
| Follower re-execute + roots | 1.0 | 3.5 | 1.0 | 1.0 | 1.0 |
| Prepare (2 barriers, sign, δ_q, verify) | 2.3 | 2.3 | 41.8 | 96.8 | 12.5 |
| Commit (2 barriers, sign, δ_q, verify) | 2.3 | 2.3 | 41.8 | 96.8 | 12.5 |
| Decision barrier | 0.2 | 0.2 | 0.2 | 0.2 | 5.5 |
| Seat → sentry | 0.3 | 0.3 | 0.3 | 0.3 | 0.1 |
| Sentry → client | 0.25 | 0.25 | 0.25 | 0.25 | 0.1 |
| Client verify + receipt barrier | 1.4 | 1.4 | 1.4 | 1.4 | 6.8 |
| **R1 p50 (model)** | **≈15 (plan with 18–20)** | **≈27** | **≈227** | **≈510** | **≈79 (≈60 folded)** |
| p95 (model) | 30–60 | ≈80 | 350–450 | 600–900 | 100–250 (flush outliers 71–225) |

**Notes.**

- **Remote client:** add its round trip and its own disk. At 250 ms RTT to a co-located cluster, expect ≈265–280 ms. A cold TLS connection adds 2 RTT (≈500 ms at 250 ms RTT), so prewarm it and report cold and warm runs separately.
- **Bursts:** a burst of 8 adds about one slot (≈8 ms co-located; ≈44 ms on the Mac). Today it adds 1.1–2.1 s.
- **Regional options [inf]:**
  - Routing clients to the leader's region saves ≈35 ms.
  - Pipelining saves ≈20 ms.
  - The gated 2δ path saves ≈40 ms.
  - Together, regional p50 could reach ≈130–170 ms.
- **The max-batch column depends on the 8,192-leaf cap.** At 32,768 leaves it would be ≈55 ms (Performance S-2).
- **Adversarial tails, before the S-3 fixes:** forged votes could push p50 to 100–130 ms. The fixes in §3.4.1 (exclude a seat after one invalid share; slow-leader reputation) target ≤ 2× idle [inf].

### 7.2 How it is measured

**Definitions.**

- M1 (`doc/flowmesh-latency-definition.md`, LOCKED) stays the historical comparator.
- R1 starts before the signing preflight and ends at a **durable** client receipt that proves the applied or open result.
- R2 is the same for a fill against pre-existing crossing liquidity, with exact fee and balance deltas. Because outcomes are inside the certificate, R2 = R1 by construction. It is still measured separately.
- R3 runs from the verified result to the Qt model and paint: p95 ≤ 30 ms. First painted book: p95 ≤ 50 ms.

**Statistics (PERF-6/7).**

- Nearest-rank; n ≥ 1,000 per arm; warm-up reported separately.
- Every failure stays in the denominator, with a 60 s deadline. A retry of the same ActionId stays inside its own sample.
- Full fsync. No async durability, no reduced quorum, no hidden pre-admission wait.

**Metadata on every run:** commit, binary SHA-256, build, host (CPU, RAM, OS, storage), fsync mode, start-up barrier probe, topology and regions, RTT matrix (median of 20 TCP connects), load, the B3-advanced flag, cgroup `nr_throttled`, and per-leader-term breakdowns.

**Gate profiles (PLAN-12, Performance P-1).**

| Profile | Setup | Pass rule |
|---|---|---|
| **G-COLO** (ruled gate) | Linux PLP, k = 4 at DEX-1, k = 64 by M7; open-loop load | R1 and R2 p50 ≤ 200, p95 ≤ 600. Internal target: p50 ≤ 50, p95 ≤ 150 |
| **G-LOAD** | 70% of declared capacity (3,500 actions/s) on the adversarial 131k-tick book | Same as G-COLO |
| **G-BLOCK** | B3 blocks injected at 1/s on regtest | p95 of trades that see a block ≤ 2 × p95 of the others |
| **G-FAULT** | Dead leader each term; one forged-vote seat; slow leader at 1.9× threshold for 1/3 of terms; kill -9 | Reported p50/p95/p99, bounded recovery, zero safety violations; no 200 ms claim |
| **G-WAN** | netem replay of the real seat RTT matrix at k = 4, 7, 64 | Reported; becomes a gate only after the geography ruling (D-G1) |
| **G-COLD / G-WARM** | First action on a new connection versus warm | Reported separately |
| Host hygiene | — | Any run with `nr_throttled > 0` is invalid |

The measured baseline remains **258–263 ms** single and **1.39 s** median for a burst of 8. The gap is architecture.

---

## 8. Security model and threats

### 8.1 Trust assumptions

| Layer | Safe while | What a breach allows |
|---|---|---|
| B3 base chain | < 1/3 of M7 finality weight Byzantine (dual quorum) | Conflicting finality; B3 halts on evidence |
| FlowMesh R1 receipts ("certified") | < 1/3 of seats Byzantine | False receipts until a checkpoint refuses them |
| FlowMesh custody ("checkpointed") | < 2/3 of registered seats collude | Redistribution of balances up to the limits; never value beyond the vault (conservation); visible to every client; delayable by pause |
| Bridge | Signers with < 2/3 of finality weight, and validity needs ≥ 50% of active stake; Ethereum sync committee honest | Forged release up to the fixed caps; disclosed (SEC-10) |
| Supply chain | Guix builds match across 2 independent builders; external audits | Malicious binary |

**Sybil cost (published).**

- Halting power at k = 64 is 22 seats ≈ 330,000 B3 at the first modern tier, or up to ≈1.3M at the 60k tier. Bought genesis FN may cost less.
- Taking 2/3 against h honest seats needs 2h+1 seats, and seat pacing (PLAN-5) spreads that over several visible epochs.
- Two AI reviewers are not independent. External audits remain mandatory.

### 8.2 Threat table

| # | Threat | Mitigation in this plan | Residual / disclosure |
|---|---|---|---|
| T1 | 2/3 seat coalition signs a false state root (Security B1) | Conservation on B3; large-withdrawal delay + `PauseV2`; `Q_min`; maturity and pacing; caps ≤ attack cost; client leaf checks | Redistribution up to caps is possible; SEC-1 re-ruled and disclosed |
| T2 | Leader or sentry withholds an action to freeze an account (B2) | Nonce window; mandatory expiry → provable `EXPIRED`; cancel by OrderId; send to 2 sentries; inclusion lists | None material [inf] |
| T3 | Leader picks slot boundaries for MEV (S1) | 8-slot terms; inclusion lists; unpredictable within-slot order; A2 pro-rata; MEV analysis as an M3 exit | Encrypted flow stays deferred (TRD-20) |
| T4 | Halting minority stops FlowMesh | Safe halt; reads live; forced exit after 10,080 blocks; exit includes reserved and futures equity | Up to ≈7 days without trading; disclosed |
| T5 | Forced exit blocked or double-spent (S2) | Height-based clock; nullifiers; resume must cover exits | Exit uses the last accepted root (see T1) |
| T6 | Bridge coalition forges a `message_root` (S3) | ≥ 50% active-stake validity floor; fixed bUSD caps; 2 h delay ≥ 50k; safe exporter | Bounded by caps until ZK; disclosed |
| T7 | Oracle manipulation for liquidations (S4) | External underlyings only; ≥ 3 sources per seat; median over all ≥ 2f+1 vote-committed submissions; staleness freeze; OI caps | Correlated venue outage; disclosed |
| T8 | V1 committee mints after P (S5) | V1 root pinned at P; V1 checkpoints rejected after P; single nullifier space | None |
| T9 | Restored backup or VM rollback double-signs (S6) | Signed high-water marks from 2f+1 seats on every start; key retirement on any gap | Needs 2f+1 reachable peers to resume |
| T10 | Admission spam from free accounts (S7) | Account must exist (deposit) before crypto; budgets scale with balance or fees; dedup after signature | Costs bounded by deposit size |
| T11 | DDoS on seats (S8) | Endpoints only inside the mesh; sentries in front; firewalled port | Sentry capacity is the limit |
| T12 | Forged-vote CPU drain (Performance S-3) | Exclude a seat after one invalid share; individual verification only when needed | Small per-epoch cost |
| T13 | Slow leader just under the timeout | Adaptive timeouts; reputation; 8-slot terms; G-FAULT profile | p95 impact reported per term |
| T14 | Fake durability (VPS write cache) | Start-up barrier probe; PLP recommended; metadata | Community seats may lack PLP; disclosed |
| T15 | Historical-validity drift in `b3d` (BIP50; btcd CVE-2022-44797) | Link, never rewrite; extended U == U′ at every pin | — |
| T16 | Transaction replay across a fork | Fork-id domain; sunset height; wire bump; observer mode | Opt-in only |
| T17 | Action replay across networks or versions | `domain` includes `ModernChainDomain`, `fm_version`, magic | None |
| T18 | Version skew | One threshold-signed manifest; lockstep handshake | — |
| T19 | Conflicting quorum certificates | Evidence + halt; "last B3-accepted checkpoint wins" | Receipts on the losing branch void; disclosed |
| T20 | Ethereum fork breaks the light client | Fork table; mint halt on unknown fork; Gloas ≥ 30 days early | Mint downtime |
| T21 | Treasury key compromise | Single key RULED; flag the growing balance | Optional multisig ruling (M5) |
| T22 | Tether freezes the reserve | None possible | Disclosed (SEC-10) |
| T23 | Trusted-facts mode reaches mainnet | Compile-time and start-up refusal for MAIN/TESTNET/SIGNET | None |

---

## 9. Milestones

[CC] milestone scopes are kept word for word. This plan's additions are listed as addenda (Feasibility F7). C1 already **passed** within its declared scope at 1f05fc74 (`src/flowmesh2/C1_RESULTS.md`).

Calendar months count from the Tier 1 rulings. All durations are estimates [inf]. They assume about a week of turnaround per owner ruling, and they treat qualification and review, not code volume, as the binding constraint (Feasibility F9/F11).

| ID | [CC] | Scope and deliverables | Exit criteria | Evidence | Weeks | Months |
|---|---|---|---|---|---|---|
| **M0** | — | Verification foundation: property catalogue v1; Linux CI box; ASan/UBSan, TSan, fuzz and lock-order jobs; evidence store with manifest tool; Guix on 2 hosts; DST simulator skeleton | Each CI job catches a **planted** defect (negative controls); a trivial DST scenario replays byte-identically twice | CI logs, planted-defect reports, Guix hashes | 4–6 | 0–1.5 |
| **B0** | part of C5 | `b3d` bounded per-block undo for derived state (also shipped to the capped V1 track) | Undo equals full replay for all depths ≤ horizon on generated and shadow chains; reorg cost O(depth); the 97 s replay is gone | Replay-equality report, timings | 4–6 | 0–1.5 |
| **M1** | C1 + addendum | C1 is done. Addendum: `nm` symbol guard, include lint, `-Werror=thread-safety` and empty-suppression TSan in Linux CI | Guards fail on a deliberate violation in a real file | CI negative controls | 1–2 | 1–1.5 |
| **M2** | C2 + addendum | Shared spot ledger and reservations; two markets on one quote AssetId. Addendum: nonce window + expiry (PLAN-1), account-exists admission (PLAN-9), Merkle-sum liability tree (PLAN-3) | ≥ 10^6 actions × 1,000 seeds byte-identical to the frozen model on Linux, macOS and Windows; conservation asserted every slot; stateful fuzz ≥ 24 CPU-h with 0 findings | Vector diff, fuzz corpus, root hashes per OS | 5–7 | 1.5–3 |
| **M3** | C3 + addendum | Order lifecycle, matching profile (A2 default + residual call), fees, caps. Addendum: within-slot order seed, 8,192-leaf cap, MEV analysis (A vs A2) | Independent checker agrees on 100% of batches, including adversarial and dust cases; apply + roots p99 ≤ 5 ms at max batch on target x86; MEV report accepted by the owner | Checker logs, perf runs with metadata, MEV report | 7–9 | 3–5 |
| **MB1** | — | `b3d` regtest consensus at P: `VaultV2`, `CheckpointV2` with conservation, caps, delay, `PauseV2`, `Q_min`, maturity and pacing, V1 root pin; trusted-facts adapter with compile guard; epoch scoping (PLAN-11) | CONS-13 checklist: activation boundaries, connect/disconnect, restart, reindex, two-node agreement, adversarial rejection (forged root exceeding the vault refused); trusted mode refuses mainnet params | Functional tests, fuzz of new codecs ≥ 24 CPU-h each | 6–8 | 3–5 |
| **M4** | C4 + addendum | Stable-leader PBFT, actual crypto, signer journal, commit log, recovery, seat mesh, sentry fan-out. Addendum: inclusion lists, 8-slot terms, adaptive timeouts, invalid-share exclusion, restart high-water | Apalache bounded check at N = 4 to depth 12; random simulation at N = 7 (≥ 10^6 traces); Twins set passes; DST ≥ 1,000 CPU-h with crashes at every barrier class; kill -9 + LazyFS: 0 equivocations, 0 conflicting certificates, exactly-once application (Feasibility F10) | Spec files, simulation seeds, independent signature-checker output | 12–16 | 4–7.5 |
| **M5** | C6 + addendum | Sentry API (HTTPS + SSE), `b3verify`, Hive trade page, outbox. Addendum: hedged dual send, prewarm, `EXPIRED` proofs, leaf-check alerts | With the chain or writer lock held for 10 s, health, status and certified reads stay within budget; R3 p95 ≤ 30 ms; first book p95 ≤ 50 ms at every handoff | Held-lock nemesis logs, Qt perf runs | 6–8 | 5–7 |
| **M6 = DEX-1** | C7 | 4-operator regtest: rUSD, deposits, B3/rUSD and ASSET/rUSD markets, limit/IOC/post-only/cancel, withdrawals through checkpoints, engine-off Qt, real fsync, live block growth, restarts | ≥ 72 h soak with forced reorgs and nemeses, 0 checker violations; G-COLO, G-LOAD and G-BLOCK pass with n ≥ 1,000; packaging (split or not) decided | Full evidence bundle; owner review | 4–6 | **7.5–9** |
| **M7** | — | Mainnet-grade adapter (finalized + 30, anchor proofs in proposals); seat registration (PoP, in-mesh endpoints, Hive validator page); state sync for joining seats; trade tape and candles; fee claims for many seats; k = 64 tests; G-WAN run | G-COLO at k = 64; G-WAN reported; a joining seat syncs and votes; [verify] the M7 finality-set lineage for non-node verification | Reports, sync tests | 10–14 | 9–12 |
| **M8** | C8 (part a) | Bridge V2 on Sepolia/Hoodi with PLAN-14 hardening; V1 cutover (claims, transfer transaction); forced exit; fork-id and sunset release; liability manifest | Foundry invariants (no duplicate mint or payout; nullifier uniqueness) with documented depth; anvil + regtest end to end; forced-exit drill; no double spend across the cutover | Invariant runs, drill reports | 12–16 | 9–13 |
| **M9** | C8 (part b) | Dated futures after the underlying/oracle ruling; oracle sidecar; liquidation index; insurance; OI caps | Economic simulation (oracle staleness and manipulation, thin-book storms) halts safely; conservation including margin in model and DST; liquidation storms stay within the slot budget | Simulation reports | 12–16 | 12–16 |
| **M10** | C5 (rest) | Separate track: finality V2 + PoS V2 fixes at height C; sign-versus-reorg proof | Sign-versus-reorg linearization proved in the spec before native signing; producer election not silently declared done; U == U′ | Spec, shadow-fork results | 12–16 | 10–15 (off the DEX path) |
| **M11** | C9 | Release qualification: external audits (book 6–12 weeks ahead, by month 10), bug bounty, Guix × 2, shadow fork, ≥ 4 weeks of observer mode, operator rehearsal | All high and critical findings fixed and re-reviewed; owner decides P (and P_f) | Audit reports, observer data | 16–24 | 16–22 |
| **V1 track** | — | Capped at ≤ 15% of effort: B0 undo plus `cs_main`-free reads for current testers (Feasibility F11) | Testers' stall regressions pass | Regression logs | rolling | 0–4 |

**Calendar.**

- First tester DEX (DEX-1): **≈7.5–9 months**.
- Mainnet decision: **≈18–24 months**.
- The critical path is M0 → M2 → M3 → M4 → DEX-1 → M8/M9 → M11. M10 does not gate P.

---

## 10. Delivery workflow

### 10.1 Roles

- **Owner:** rules on decisions, controls the gate files, merges. Only the owner merges or pushes.
- **Codex:** implements in small, buildable commits on the owner-designated branch (today `flowmeshV2-dev`, under `src/flowmesh2/`). No AI attribution. No push. No history rewrite.
- **Claude:** audits each change packet and each milestone in a separate context. Claude never certifies its own code.
- **External auditors:** required before activation. Two AIs are not independent.

### 10.2 The change packet

| Step | Actor | Output | Mechanical check |
|---|---|---|---|
| 1. Ticket | Owner / Claude | Requirement IDs, PLAN IDs, property IDs, acceptance criteria, affected LOCKED items (must be none or ruled) | Template lint |
| 2. Spec delta | Claude drafts; the owner rules if semantics change | Quint/TLA+ diff or model/vector diff | Apalache/Quint run attached |
| 3. Red test | Codex | A failing test (unit, vector, simulation seed or fuzz input) | CI runs it on the parent and expects failure |
| 4. Implementation | Codex | Small commits | Every commit builds and tests |
| 5. Gates | CI | L0–L6 as applicable (below) | A deliberately racy or UB canary must be flagged, proving the sanitizers ran |
| 6. Evidence | CI / Codex | Bundle under `/Users/josh/development/ON/B3-FlowMesh-200ms-evidence/<milestone>/<change>/<run>/` with `run.json`, logs, traces, kept failures, checker verdict and `MANIFEST.sha256` | Auditor recomputes the manifest |
| 7. Audit | Claude | Verdict per criterion (met / partly / not met), each with evidence links | See checklist |
| 8. Ruling | Owner | Accept / accept with fixes / reject; merge | Recorded in the ruling ledger |

### 10.3 CI layers

| Layer | Content | When |
|---|---|---|
| L0 Static | Clang thread-safety as errors; lint bans; `nm` and include guards | Every commit |
| L1 Unit/property | Boost.Test; small-domain exhaustive rounding tests | Every commit |
| L2 Vectors | Python models → canonical vectors → C++; independent checker | Every commit |
| L3 Spec | Quint/Apalache bounded checks; trace validation | PR (short), nightly (deep) |
| L4 DST | Seeded whole-cluster simulation; swarm; fault injection at barriers; Twins; ByzzFuzz-style mutations | PR ≈4 CPU-h; nightly ≥ 256 CPU-h; ≥ 1,000 CPU-h before a gate |
| L5 Fuzz | libFuzzer + ASan/UBSan, AFL++; every decoder has a `FUZZ_TARGET` | PR smoke 60 s per changed target; nightly all; ≥ 24 CPU-h per new consensus target before a gate |
| L6 Sanitizers | ASan, UBSan, TSan (no suppressions), `DEBUG_LOCKORDER` | Nightly; before merging concurrency changes |
| L7 Real processes | Linux harness: kill -9, LazyFS, partitions, clock skew, 72 h soak with forced reorgs | M4 onward |
| L8 Performance | Open-loop generator; gate profiles of §7.2; same-host A/B | Each milestone; Qt at every handoff |
| L9 Release | Guix × 2, audits, bounty, shadow fork, rehearsal | M11 |

### 10.4 Claude's milestone audit checklist

1. **Build:** clean checkout at the exact commit; record the binary hash.
2. **Rerun:** every test and gate. Recompute the evidence manifest.
3. **Confirm the gates ran:** negative controls fire; sanitizer canaries are flagged.
4. **Attack:** new DST seeds, Twins and mutants on the diff; nemesis runs; fuzz with new seeds.
5. **Trace:** every PLAN, requirement and property ID in the ticket has evidence.
6. **Check invariants:**
   - no `cs_main` symbols; no `std::mutex`;
   - no floats, wall clock or unordered iteration in state code;
   - no LOCKED change without a ruling.
7. **Check performance claims:** PERF-6/7 metadata complete; failures in the denominator; `nr_throttled = 0`.
8. **Check process:** CLAUDE.md rules (branch, no push, no AI attribution, small commits); no mainnet contact.
9. **Report:** solve-and-report format (issue → change → reason), with every failure kept.

### 10.5 Protected gate files

These are owner-controlled: CI configs, test floors, the property catalogue, performance thresholds, sanitizer suppressions, fuzz dictionaries and the MPA/wire registry. Codex may **add** tests. Removing or weakening any of these needs an audit plus owner sign-off.

---

## 11. Risks and mitigations

| # | Risk | Likelihood / impact | Mitigation |
|---|---|---|---|
| 1 | Ruling backlog stalls work (about 75 decisions exist; ≈2.4 rulings/week historically) | High / high | Three tiers; only 9 now; no RE-RULE added to the DEX path beyond Tier 1–2 |
| 2 | Seat coalition steals within limits | Medium / high | PLAN-2..6; disclosure; caps sized to attack cost |
| 3 | WAN seat set cannot meet 200 ms | High / medium | Geography ruling D-G1; publish modelled and measured G-WAN; options of §7.1 |
| 4 | Agreement or recovery bugs | Medium / critical | Spec first; Apalache; Twins; DST ≥ 1,000 CPU-h; LazyFS; independent checker |
| 5 | Historical-validity drift in `b3d` | Low / critical | Link, never rewrite; extended U == U′ at every pin |
| 6 | Fake durability on VPS hosts | Medium / high | Barrier probe; PLP qualification; metadata |
| 7 | CPU throttling causes freezes (100 ms boundaries seen) | High on VPS / medium | No busy-polling; `cpu.max` sizing; pinning; runs with `nr_throttled > 0` invalid |
| 8 | Futures oracle or liquidation losses | Medium / high | External underlyings only; dated first; caps; insurance; deficit halt |
| 9 | Bridge: B-22 committee, Gloas fork, Tether freeze | Medium / high | Validity floor; fixed caps; fork table; disclosure |
| 10 | Chain split at P with non-upgraded producers | Medium / high | Sunset release early; observer mode; ≥ 90% stake; fork-id |
| 11 | Version skew between `b3d` and FlowMesh | Medium / medium | One manifest; lockstep handshake; mixed versions refused |
| 12 | External audit lead time (6–12 weeks, likely six figures) | High / schedule | Book by month 10; scope early |
| 13 | Testers lose interest during 7–9 months | Medium / medium | Capped V1 track; B0 fix shipped early |
| 14 | MSVC arithmetic divergence on Windows clients | Low / medium | mingw-w64 release; MSVC vectors in CI |
| 15 | LevelDB compaction stalls leak into tails | Medium / low | Snapshots only; separate device; level-0 monitoring |

---

## 12. Owner decisions needed

### 12.1 Tier 1: needed now, before code beyond M1

| # | Decision | Recommended default | Why now |
|---|---|---|---|
| D1 | **G0:** new core on the same B3 chain (Path A) or a new hub (Path B)? | **Path A**, with the chain adapter keeping Path B possible | Everything depends on it |
| D2 | **SEC-1 re-rule (PLAN-2):** accept "2/3 of registered seats can move value up to published limits; B3 refuses inflation; pause window"? | **Accept**, with PLAN-3..6 and disclosure. Research ZK or fraud proofs as a later track | The alternative (B3 re-executes FlowMesh) is a different architecture |
| D3 | **C-1 reconciliation commit:** stable quote and fees in Q | **Land** the [SQ] Appendix A draft as a reviewed commit | CLAUDE.md requires it before implementation |
| D4 | **Matching (R-D3/C-4, RE-RULE of D-1):** A2, A or B? | **A2** (price-time makers; same-slot takers pro-rata per level; residual call); the MEV analysis may switch to A | M3 builds it |
| D5 | **Fee incidence (C-2)** | **Seller pays 100 ppm once in Q** | M3 |
| D6 | **Lot/tick (OD-7) and engine caps** | Per-market integer tick and lot at listing; the ±50% band fits 131,072 ticks; caps as in §3.5, including the 8,192-leaf cap | M2/M3 |
| D7 | **Nonce window and expiry (TRD-18 RE-RULE, TRD-19, G5)** | 100-nonce window; `max_delay ≤ 1,024` slots; cancel by OrderId; exact-byte resend allowed; `EXPIRED` provable | M2 |
| D8 | **Sequencing:** DEX-1 (C7) before C5, on regtest trusted facts | **Yes** (OPS-2); trusted mode compile-guarded off mainnet | The whole calendar |
| D9 | **Dev tools and CI hardware** | Approve Quint + Apalache, LazyFS, AFL++ (dev-only), and a ≥ 32-core Linux PLP box | M0 |

### 12.2 Tier 2: before the 4-operator regtest (≈month 4–5)

| # | Decision | Recommended default |
|---|---|---|
| D10 | R-D1 agreement family | Stable-leader PBFT ([CC] C4) |
| D11 | FN-10 leader rule (RE-RULE) | 8-slot terms, seed-chosen, slow-set skip |
| D12 | C-10 seat port | Separate port 5649, firewalled |
| D13 | SEC-3 budgets (RE-RULE) | Protocol-derived seat budgets; HL-like account credits |
| D14 | C-20 registry | One append-only MPA/wire registry file, owner-assigned |
| D15 | Barrier folding | Strict for DEX-1; measure folded; decide at DEX-1 |
| D16 | Regtest values of `Q_min`, caps, delays | `Q_min` = 4 on regtest; mainnet value in Tier 3 |
| D17 | Throughput target (PERF-14) | 5,000 actions/s sustained; 20,000/s for 1 s |

### 12.3 Tier 3: after DEX-1, before the mainnet milestones

| # | Decision | Recommended default |
|---|---|---|
| D-G1 | Seat geography and a WAN gate | Publish the G-WAN result. Choose between a recommended regional core (best latency) and accepting a WAN p50 of 250–600 ms. Never mandate co-location (DEC-4). |
| D18 | Mainnet `Q_min`, maturity, pacing, caps, large threshold | `Q_min` = 16; maturity 1,440 blocks; pacing max(4, ⌊k/4⌋) per epoch; pilot 10k/day; 50k threshold |
| D19 | `PauseV2` signers | ≥ f+1 seats **or** ≥ 1/3 of finality weight; ≤ 72 h per 7 days |
| D20 | Forced exit (PLAN-16) and the false-root remedy | 10,080-block height clock; exit base = last accepted root (alternative base touches SEC-2) |
| D21 | Futures underlying, oracle sources, sidecar; release shape (C-5) | BTC/Q and ETH/Q, dated; ≥ 3 named venues; futures at P_f in the same binary |
| D22 | C-9 anchor | Keep 30 at launch; consider the finalized pin after M10 |
| D23 | C-6 bridge text; B-22 committee; B-23 stranding; PLAN-14 floor and caps | Reconcile to R3/R5; ≥ 50% active-stake validity floor; fixed caps |
| D24 | Fork-id and sunset height | Opt-in fork-id at P; sunset release ≥ 3 months before P |
| D25 | R-D6 cutover; V1 root pin (PLAN-15) | Parallel claims plus transfer-by-B3-transaction |
| D26 | C-12 / D3 / D4 (PoS V2 scope) | Fix the stuck set and headcount Sybil at C; no slashing in the first release |
| D27 | k > 128 committee rule | Decide only if registrations approach 128 |
| D28 | Q16/Q17 sponsor; Q24–Q57 assets and listing; OD-3 governance; R-D7 hardware | After DEX-1 (OPS-2) |
| D29 | Optional: treasury multisig (M5) | Reconsider, given the growing balance |
| D30 | After V1: agent keys (UX-9) | Trade-only, revocable, per-agent nonce windows |

---

## 13. What we explicitly do NOT do

- **No new genesis, snapshot or balance migration.** No freeze-and-claim (INV-1/3/14).
- **No rewrite** of Bitcoin cryptography, serialization, validation or the B3 legacy rules (IMPL-1). No libbitcoinkernel or libmultiprocess for now.
- **No Rust**, no new hot-path database, and no WebSocket dependency (SSE is used instead).
- **No Simplex, two committee tiers, 2δ fast path or pipelining** in the first DEX. Each is reconsidered only on measured need.
- **No process split for DEX-1** unless the DEX-1 review criteria fail.
- **No `facts_root`, finality V2 or PoS V2 on the DEX critical path.**
- **No perpetuals first**, no funding, no ADL, no cross or portfolio margin, no house vault, and no B3/Q futures until external liquidity exists.
- **No shortcuts to meet latency:** no async durability, no reduced quorum, no hidden pre-admission waits, and no WAN claims made by adding delay to a local median.
- **No Hyperliquid copies:** no mandated co-location, broadcaster whitelist, closed binaries, auto-upgrade or stop-the-world weekly upgrades.
- **No discretionary rollback, bailout or refund** (SEC-2).
- **Bridge:** no signer-attested minting, no LP, no Arbitrum vault, no USDC.
- **No slashing of STAKE** in the first release.
- **No agent keys or web app** before V1.
- **No unlabelled states.** "Certified", "checkpointed" and "B3-final" are never shown as one state.
- **No PLP requirement for community seats.** It is recommended and disclosed.
- **No mainnet contact** during development, no push by AI, no AI attribution, and no deletion of journals, wallets, orders, claims or tester datadirs.

---

## Appendix A. How the critiques were applied

### A.1 Blocking items (all applied)

| Item | Applied as | Where |
|---|---|---|
| **Security B1:** two tiers add no custody protection; "keyless" overstated | Two tiers dropped. SEC-1 disclosed and sent to the owner as D2. B3-checked Merkle-sum conservation; 360-block large-withdrawal delay and bounded `PauseV2`; `Q_min`; seat maturity and pacing; caps ≤ attack cost; client leaf checks | §3.6, PLAN-2..6, §8 T1, D2, D18, D19 |
| **Security B2:** strict nonces let a leader or sentry freeze an account | Nonce window of 100; mandatory `ref_slot + max_delay`; provable `EXPIRED`; cancel by OrderId; G5 defaulted; inclusion lists; dual-sentry send. Placed in M2, ahead of C3 | §3.6, §3.8, PLAN-1, D7 |
| **Performance P-1:** gate does not test the deployment; WAN cherry-picked | Gate profiles G-LOAD, G-BLOCK, G-FAULT, G-WAN, cold/warm; per-term reporting; global and regional columns in the budget; geography ruling D-G1 | §7, PLAN-12, D-G1 |
| **Performance P-2:** epoch bump on every tip recreates the block stall | Epoch changes only on seat-set change or reorg at or below the anchor; 2 s anchor hysteresis; anchor proofs in proposals; G-BLOCK gate | §3.3, PLAN-11 |
| **Performance P-3:** slot caps exceed leader upload; ID-only proposals add a fetch | Sentry fan-out to all seats; 6-byte short IDs; full bodies for fresh actions; byte cap `B_link × T_slot / (k−1)`; throughput target | §3.4.1, PLAN-10, D17 |
| **Feasibility F1:** first mainnet DEX depends on a hard fork | P carries only the FlowMesh rules. No `facts_root`. Seats use their own `b3d` plus anchor proofs. Bridge Verifier V2 on the existing M7 gadget. Bounded undo in month 1. C is a separate track | §1.1, §3.3, §3.9, §6.2, B0, M10 |
| **Feasibility F2:** ruling demand exceeds bandwidth | Three tiers: 9 now, 8 before regtest, the rest after DEX-1 | §12 |
| **Feasibility F3:** futures without an oracle source | Underlying and sources ruled before M9 design (D21); external underlyings only; C8 split into M8 and M9; release-shape question to the owner | §3.11, M8, M9, D21 |

### A.2 Should-fix items

| Item | Disposition | Where |
|---|---|---|
| Security S1 (leader slot boundaries, MEV) | **Accepted.** Inclusion lists; 8-slot terms; unpredictable within-slot order; MEV analysis as an M3 exit | §3.4.1, §3.5 |
| Security S2 (forced-exit gaps) | **Accepted.** 10,080-block height clock; exit includes reserved and futures equity; client proofs; resume covers nullifiers. False-root exit base left OPEN (D20) | §6.5 |
| Security S3 (bridge bounds) | **Partly accepted.** Validity floor ≥ 50% of active stake; fixed bUSD caps; disclosures. **Deferred:** the bond/limit auto-pause (needs an in-consensus price) and bonds themselves (C track) | §3.9 |
| Security S4 (self-referential oracle) | **Accepted.** External underlyings only; ≥ 3 sources; median over all vote-committed submissions; OI caps | §3.11 |
| Security S5 (V1 authority never frozen) | **Accepted.** V1 root pinned at P; transfer as a B3 transaction | §6.4 |
| Security S6 (restore detection) | **Accepted.** High-water marks from 2f+1 seats on every start; key retirement on gaps | §3.7 |
| Security S7 (admission Sybil) | **Accepted.** Account must exist before crypto; scaled budgets; dedup after verification | §3.8 |
| Security S8 (endpoint target list) | **Accepted.** In-mesh endpoints only; sentries in front | §3.8 |
| Security M1–M4, M6, M8 | **Accepted** | §3.4.1, §6.3, §3.6, §3.3, §3.8 |
| Security M5 (treasury multisig) | **Deferred** to an optional owner ruling (ECO-3 stands) | D29 |
| Security M7 (slashing contradicts CONS-1) | **Accepted.** No slashing in the first release | §3.4.2 |
| Performance S-1 (crypto and re-execution missing) | **Accepted.** Budget rebuilt with BLS and re-execution | §7.1 |
| Performance S-2 (Merkle cap) | **Accepted.** 8,192-leaf cap unless parallel hashing passes on x86 | §3.5 |
| Performance S-3 (tails) | **Accepted.** Invalid-share exclusion, reputation, G-FAULT, p95/p99 published | §3.4.1, §7.2 |
| Performance S-4 (timeouts) | **Accepted.** SRTT + 4·RTTVAR with floors; term ends at its first view change | §3.4.1 |
| Performance S-5 (CPU quotas) | **Accepted** | §3.2, §7.2 |
| Performance S-6 (network tails) | **Accepted.** `rto_min`, slow start, hedging, prewarm, cold/warm reporting | §3.8, §7.2 |
| Performance S-7 (single-leader forwarding) | **Accepted.** Fan-out to all seats; re-propose at term change | §3.4.1 |
| Performance S-8 (hidden pauses) | **Accepted.** Read replica fed from the log; liquidation-price index | §3.2, §3.11 |
| Performance S-9 (storage details) | **Accepted.** Zero-filled segments; LevelDB off the log device | §3.7 |
| Performance M-1 (pipelining) | **Deferred** until G-WAN shows need | §13 |
| Performance M-2 (barrier folding) | **Accepted as an option** (D15). Strict for DEX-1 | §3.7 |
| Performance M-3 (client receipt barrier) | **Partly accepted.** Outbox group-commit yes; the barrier stays in R1 because R1 is defined as durable | §3.7 |
| Performance M-4, M-5 | **Accepted** | §3.2 |
| Feasibility F4 (Simplex) | **Accepted.** PBFT default | §3.4.1 |
| Feasibility F5 (process split) | **Accepted.** In-process for DEX-1, with split criteria | §3.2 |
| Feasibility F6 (two tiers premature) | **Accepted.** DEC-5 kept; designed for k ≤ 128 | §3.4.1 |
| Feasibility F7 (milestone ID collision) | **Accepted.** [CC] scopes kept; addenda listed | §9 |
| Feasibility F8 (C5 before C7) | **Accepted.** DEX-1 on trusted facts (D8); B0 in month 1 | §9 |
| Feasibility F9 (uncosted work) | **Accepted.** Simulator, state sync, tape/candles, seat registration, fee claims and audit lead time added to M0, M7, M11 | §9 |
| Feasibility F10 (unattainable exits) | **Accepted.** N = 4 bounded, N = 7 random simulation; tools requested in Tier 1 | M4, D9 |
| Feasibility F11 (testers wait) | **Accepted.** Capped V1 track | §9 |
| Feasibility M1–M4 | **Accepted.** Calendar recomputed; PLP recommended, not mandatory; SSE; separate log device optional | §9, §3.7, §3.8 |

---

## Appendix B. Sources

- **Baseline and designs (greenfield folder):** `requirements.md`, `synthesis.md`, `design-A-continuation.md`, `design-B-new-l1.md`, `design-C-split.md`, `research-bitcoin-components.md`, `research-consensus.md`, `research-engine.md`, `research-storage.md`, `research-network-api.md`, `research-verification.md`, `research-migration.md`, `research-bridge.md`, `critique-security.md`, `critique-performance.md`, `critique-feasibility.md`, `latmodel.py`, `probes/`, `bench/`.
- **Governing (read-only):**
  - `/Users/josh/development/ON/B3-FlowMesh-hl/CLAUDE.md` and `doc/design/*` as listed in `requirements.md` §8.
  - `flowmeshV2-dev:doc/design/b3-flowmesh-clean-core-20261016.md` and `flowmeshV2-dev:src/flowmesh2/C1_RESULTS.md` (read with `git show`, a64ea997 / 1f05fc74).
- **Evidence:** `/Users/josh/development/ON/B3-FlowMesh-200ms-evidence/` (`audit-codex-overnight-20261016/audit-report.md`, `hyperliquid-research-20260928/hyperliquid-learnings-report.md`, `v2-stable-quote-20260930/b3-flowmesh-v2-stable-quote-spec.md`, `redesign-review-20261015/`).
- **Memory:** `/Users/josh/.claude/projects/-Users-josh-development-ON-B3-CoinV2/memory/` (rulings ledger in `requirements.md` §7).
- **External primary sources:** cited inside the research files with access dates, including:
  - PBFT, OSDI '99;
  - Simplex, ePrint 2023/463;
  - Twins (Bano et al.);
  - ByzzFuzz, OOPSLA 2023;
  - RFC 6962;
  - BIP152, BIP324, BIP340, BIP113;
  - kernel.org ip-sysctl (fetched 2026-10-16 in the performance critique);
  - the Hyperliquid documentation.

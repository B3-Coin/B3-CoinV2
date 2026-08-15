# B3 Implementation Status / Gap Matrix

Status of the tree against [b3-architecture-contract.md](b3-architecture-contract.md).
Baseline: commit `a8ad010` (the tip of the completed experimental stack, from which
`claude/b3-clean-architecture` is branched).

> **Phase-model note (2026-08-16).** The transition architecture now inserts a
> **temporary-PoW corridor** between legacy PoS and modern PoS
> ([b3-during-fork-transition.md](b3-during-fork-transition.md)): H+1…H+1000
> are modern-format PoW blocks (Policy Outputs active, STAKE outputs created
> and matured); modern PoS begins at M = H+1001. Everything below that says
> "modern" for heights immediately after H refers to the modern *format*;
> the modern *PoS* rows now bind at M. Current code and tests still encode
> the two-phase model (H+1 = modern PoS) — the exact contradiction register
> is corridor doc §11 and is deliberately unresolved until the corridor
> design is approved for implementation. In particular the H+1 fail-closed
> integration expectation (`no-modern-pos-rules`) will eventually move to
> the first attempted M-block, and a future `ConsensusPhase`
> {LEGACY_POS, TRANSITION_POW, MODERN_POS} abstraction will replace boolean
> era checks for block production. Step 3 of the critical path acquires a
> corridor-validation stage (scrypt work check + corridor difficulty on
> modern-format blocks) before modern PoS.

**Legend**

| Mark | Meaning |
|---|---|
| LOCKED | Decision recorded; no code owed yet |
| IMPLEMENTED | Built and tested, matches the contract |
| PARTIAL | Built but incomplete or unwired |
| WRONG | Built but contradicts the contract |
| MISSING | Not built |
| BLOCKER | Security/correctness defect that gates activation |

---

## 1. Transition anchor and finality (contract §2–5)

| Item | Status | Evidence / gap |
|---|---|---|
| Enter modern only if `height==H && hash==X` | IMPLEMENTED | `src/consensus/boundary.h` `CheckLegacyBoundaryHeader`; dispatched in `ContextualCheckBlockHeader` (`src/validation.cpp`). H/X unset on mainnet, per §2. |
| H is a hard finality boundary | IMPLEMENTED | **D1 fixed.** `Consensus::ReorgFromForkCrossesLegacyBoundary` (`src/consensus/era.h`) asks by fork point; `FindMostWorkChain` discards prohibited candidates so selection never demands a refused disconnect, and `ActivateBestChainStep` treats a late-appearing candidate as a rejected chain rather than `FatalError`. Blocks are not marked invalid — only unusable as a tip. Predicate covered by `legacy_boundary_tests/candidate_forking_below_H_is_refused_by_fork_height`. **Still owed:** an integration test driving a real reorg attempt through `ActivateBestChain` (see below). |
| Legacy fork after H is dead | PARTIAL | Codec-legality rejection works (`Consensus::HasExpectedB3BlockCodec`). "No work/stake competition" leaks via D2 below. |
| Consensus hard-switch at H+1; grace is operational only | LOCKED | Consensus behaviour correct. Wallet/relay grace tooling not built (post-H+1). |
| No consensus fallback to legacy blocks | LOCKED | Nothing implemented; correct. |

## 2. Codec, identity, hash domains (contract §6–9, 14)

| Item | Status | Evidence / gap |
|---|---|---|
| Marker-driven identity before parent height | IMPLEMENTED | `CBlockHeader::GetMarkerHash` (`src/primitives/block.cpp`) used at every identity site; the earlier height-0 assumption is removed. Frozen vectors in `src/test/legacy_identity_tests.cpp` prove a guessed height yields the wrong identity. |
| Marker/height legality table | IMPLEMENTED | `src/consensus/block_codec.h`, enforced in `ContextualCheckBlockHeader` and re-checked in `ConnectBlock`. |
| Legacy/modern hash domains separated | IMPLEMENTED | Legacy = historical scrypt domain; modern = existing modern domain. **No tagged modern block hash is to be added** (contract §7). |
| Domain tags on newly introduced hashing | PARTIAL | Assets and FlowMesh use explicit tags (`"b3/asset/v1"`, `"b3/flowmesh/*"`). Policy commitment preimages are not yet standardized (contract §25). Applies to new constructions only — not block or existing tx identity. |
| Legacy UTXO identity preserved | IMPLEMENTED | Provenance hashing via `CTransaction::m_legacy_encoding`; historical txids/outpoints byte-exact. |
| Standalone tx codec from trusted per-peer context | IMPLEMENTED | `PeerManagerImpl::UsesLegacyWireTransactions` / `Peer::m_legacy_protocol`. **No transaction-family byte is to be added** (contract §8). |
| No process-global era switch | IMPLEMENTED | Stream-parameter codec selection; no `fModernMode`. Compile-time separation of legacy/modern objects is weaker than ideal but no global mutable selector exists. |

## 3. Legacy era and trusted replay (contract §10–13, 16)

| Item | Status | Evidence / gap |
|---|---|---|
| Legacy consensus fidelity | IMPLEMENTED | `src/legacy/consensus.cpp`, `src/legacy/pos.cpp` — kernel preimage, stake modifier ordering, reward ladder, difficulty EMA, FN collateral tiers and the in-block tx offset base all verified against the `master` client. Real mainnet vectors: genesis and the block-136 kernel. |
| Replay pipeline | **WIRED (node connect path)** | `legacy::TrustedReplay` / `PersistentReplay` implement parse/linkage/checkpoint/Merkle/apply with the correct skip list, atomically and crash-safely, and the engine can emit standard `CBlockUndo` data (per non-coinbase tx, spent coins in input order) so replay-connected blocks disconnect through the ordinary undo path. **The node now uses the engine in production**: with the boundary pinned (`Consensus::LegacyBoundaryPinned` — both H and X configured, the shipped post-fork configuration), `ConnectBlock` dispatches every legacy block to `TrustedReplay` (checkpoints = the historical map plus `{H: X}`, position derived from the block index), never the live branch — so the state a syncing node builds is by construction the state `b3coin-utxo-verify` attests. Admission is replay-scoped in the same mode: codec/marker legality, the exact boundary identities, hardened checkpoints and the physical future-time bound are kept; the version whitelist, past-time/median-time rules, PoW (accept-time and `ReadBlock` read-time), retarget conformance, block signature, transaction-time rule, the connect-time contextual set and the rolling depth bar are live-only (unpinned) rules. The live per-index bookkeeping (stake modifiers, hash proofs, money supply) is intentionally not populated for replayed history. Because pinned admission no longer re-judges claimed difficulty, a fabricated high-work chain can transiently become the active tip before X is known; `Chainstate::AbandonOffAnchorTip` (invoked from `ActivateBestChain`) unwinds a tip proven off-anchor once X's index entry exists — InvalidateBlock-style, marking the blocks `BLOCK_ANCHOR_INELIGIBLE` (side history, not invalid), refusing to resurrect their transactions, then repopulating the candidate set and recomputing the best header so the attested chain activates despite carrying less recorded work. Tested end to end in `legacy_transition_tests`: replay-scoped admission (`pinned_admission_stops_re_judging_live_rules`), replay connection incl. live-invalid-but-attested blocks and mechanical rejections (`pinned_boundary_blocks_connect_through_replay`), and fake-chain-first poisoning recovery (`off_anchor_active_tip_recovers_once_x_is_known`). Remaining: no dedicated init-time/bulk-replay front-end (`PersistentReplay` is used by the standalone tool; the node reconstructs through the ordinary sync/connect machinery), no RPC surface. |
| Historical checkpoint anchors + rolling depth bound | DONE (live legacy) | The 13 historical checkpoints and `nCheckpointSpan = 500` are ported verbatim from `master` into `legacy::MainnetCheckpoints()` / `legacy::LEGACY_CHECKPOINT_SPAN`, carried on `Consensus::Params` (`legacy_checkpoints`, `legacy_checkpoint_span`) and installed into `CMainParams`. Enforced in `ContextualCheckBlockHeader`'s legacy branch (the accept-time analog of master's `AcceptBlock`, before `AddToBlockIndex`): `CheckpointAllows` (hard, DoS-100 `bad-legacy-checkpoint`) and `ReorgDepthExceeded` (no-penalty `legacy-reorg-too-deep`, `BLOCK_HEADER_LOW_WORK`). The depth rule is skipped while `LoadingBlocks()` (reindex/import may read valid deep history out of order) and, being in the live-validation path, is never consulted by `TrustedReplay`. Three modes are separated and tested: **pre-X live legacy** enforces both rules; **post-X trusted replay** reconstructs the canonical prefix with no depth rejection (a deep block is not "deep" to replay); **modern** skips both (era-gated). Unit tests (`legacy_checkpoint_tests`) pin the map and boundaries; an integration test (`legacy_transition_tests`) proves the three-mode split, including that a deep fork live legacy refuses does not break replay of the canonical X-anchored history. Replay still populates its own configured checkpoints separately (contract §11 step 5). |
| Replay == legacy state equivalence test | PARTIAL (framework done; **three-way real-data run pending — activation gate**) | **The invariant is three-way** ([b3-utxo-equivalence.md](b3-utxo-equivalence.md)): `U_master(T) == U_port(T) == U_replay(T)`. The original tool comparison establishes only `U_port == U_replay` — the port's own live-validation chainstate vs the port's own replay, two reimplementations that cannot detect a shared porting divergence from the historical client. The reference side is now in place: branch `claude/b3-master-utxo-export` (on the legacy `master` codebase) adds an `-exportutxo=<file> -exportutxoat=<X>` init mode that walks the old client's own LevelDB transaction index with the old client's own deserialization and emits the canonical logical row file (`b3-utxo-rows/v1`: outpoint, raw amount, exact script bytes, height, coinbase/coinstake flags, tx time, in-block offset; sorted by raw outpoint bytes; byte-identical across producers; never raw DB serialization). `b3coin-utxo-verify` gained `-portrows=`/`-replayrows=` (canonical row export of its two states) and `-masterrows=` (ingest the reference export, verify the full three-way invariant, report per-row differences; exit 0 only when all three agree). Mismatches are diagnosable by canonical rows, not commitment alone. **GATE: H/X must not be pinned into mainnet params — i.e. the pinned-mode replay path stays dormant — and no init-time replay wiring may be added until the three-way comparison passes on real B3 history at the candidate (H, X).** The master exporter is **compiled and smoke-verified**: full `b3coind` (export branch, unmodified legacy sources + a documented `makefile.osx-arm64` variant) links against source-built OpenSSL 1.0.2u / BDB 4.8.30.NC / boost 1.63 / vendored LevelDB; offline runs confirm the tip-mismatch refusal and a well-formed zero-row export at the genuine B3 genesis. The export branch also carries `-exportstopatheight` capture tooling (freeze the datadir at exactly T). The audited membership predicate, the verified build recipe, the three mandated capture heights (checkpoint 95350; 110000 covering the repair-window/restricted/superblock region; a recent well-buried height), the bisect procedure and the row-mutation negative test are locked in [b3-utxo-equivalence.md](b3-utxo-equivalence.md); executing the captures on real history is the operator step. Original two-way tool detail: deterministic canonical UTXO-equivalence tool (`src/node/utxo_commitment.{h,cpp}`, non-consensus): `UtxoSetCommitment` folds each outpoint plus the **exact Coin contents** (value, script, height, coinbase/coinstake flags, legacy `nTime`/`nTxOffset`) in canonical outpoint order — not aggregate supply; `CompareUtxoViews`/`CompareUtxoSets` produce a commitment per side and per-outpoint mismatch diagnostics (one-sided or differing). Unit test (`utxo_commitment_tests`) proves it detects every mismatch kind including each Coin field; the transition fixture now proves **full-set** `U == U'` (trusted replay of genesis..H vs the fully-validated live chainstate) by equal commitment with zero mismatches. **Operator command shipped: `b3coin-utxo-verify -datadir=<dir> -height=<H> -hash=<X>`** (`src/bitcoin-utxo-verify.cpp` + core `src/node/utxo_equivalence_check.{h,cpp}`) — against a cleanly-stopped node (synced with `-stopatheight=H`): refuses non-existing databases (never creates or writes into the datadir), loads the block index read-only, walks the X-anchored chain to genesis, reads raw legacy blocks (xor-aware, each re-verified against its indexed marker hash), replays via `TrustedReplay` into a disposable scratch DB, verifies the chainstate best block is exactly X at H, compares the full sets, prints counts, both commitments and bounded per-outpoint diagnostics; exit 0 only on `U == U'`, 1 on any difference or verification failure, 2 on usage/environment errors. Outside consensus and startup by construction. **Still pending (needs real data, cannot run offline): running it against a real synced datadir at the candidate H** — an operator step. |
| Mempool era rules + boundary flush | DONE | Admission is defined by the **era of the next block** (`active_tip_height + 1`) at the single choke point every path funnels through (`MemPoolAccept::PreChecks`): pre-H only legacy-encoded transactions, validated under legacy next-block rules (`CheckLegacyTxInputs` — coinbase+coinstake maturity, input-time rule, proof-of-integration fees; standalone coinstake refused as in the historical client; consensus script caching selects the next block's flags, so a modern tx at tip==H is never cached under witness-free legacy flags); post-H only modern-encoded. Provenance = decode context (per-peer wire codec / block codec / local construction), never reinterpreted; no global codec switch, no family byte. Connecting H **atomically empties the pool** under `cs_main` + the mempool lock. `mempool.dat` format 3 carries per-tx codec provenance, so identity survives a dump/load and a pre-H file can never repopulate a post-H node (the gate refuses each entry on reload; formats 1/2 remain readable). Reorg resurrection passes through the same gate; the reorg maturity filter applies coinstake maturity in the legacy era; a reorg across H cannot resurrect legacy txs because it cannot happen (D1). Pre-H RPC/wallet submissions decode modern and are cleanly refused — legacy submission over RPC is explicit follow-up work. Boundary-tested H-1→H→H+1 in the transition fixture, incl. persisted entries and within-era resurrection. |
| Historical reward-cap exception | DONE | Provenance investigation resolved the flat exception into **three sourced rules**, now ported faithfully (see [b3-legacy-fork-choice.md](b3-legacy-fork-choice.md) "Historical reward-rule exceptions"): (1) the repair window 77447–77505 (verbatim in the final client, kept as `legacy::IsRepairWindowHeight`); (2) height 107488 was **not** a cap bypass but a structured superblock rule — last coinstake output ≤ 75,656,908 × 1e9 units to the pinned P2PKH payee — the former blanket bypass dropped that validation, now enforced (`bad-cs-superblock`, params `legacy_superblock_height/pubkey`); (3) a previously **missing** unconditional rule: above height 78000 a coinstake whose second output pays the restricted destination (`ShJsVNBQ…`, hash160 `db8c…b0a5`) with positive reward is rejected (`bad-cs-restricted`), using the 0.8-era ExtractDestination fold (P2PK→key-hash) that modern ExtractDestination lacks. The FN-payment check is deliberately not ported (gated on `!IsInitialBlockDownload()` in the final client — not a sync-consensus rule — and barred by the no-FN scope decision). Trusted replay never adjudicates rewards, so it is unaffected. Golden-vector unit tests in `legacy_pos_tests`. |
| Frozen legacy script ruleset | PARTIAL | `SCRIPT_VERIFY_LEGACY_B3_STRICTENC` and the legacy branch of `GetBlockScriptFlags` exist, but there is no explicit frozen `FINAL_LEGACY_RULESET`, and the modern spend path does not yet select frozen legacy flags (contract §16). |

## 4. Legacy-spend bridge and modern model (contract §15, 19–27)

| Item | Status | Evidence / gap |
|---|---|---|
| `LEGACY_LOCK` view of a legacy prevout | PARTIAL | `modern::ViewLegacyCoin` is exactly the required projection and is proven non-mutating, but is **not wired** into any modern spend path. |
| Native B3 reserved asset id | IMPLEMENTED | `modern::NativeAsset()` (all-zero), never issued via the generic engine. |
| Modern output model | IMPLEMENTED | `modern::ModernOutput` matches the contract, plus the ratified `policy_params` field. |
| Policy type coverage | PARTIAL | Only `LEGACY_LOCK/OWNER/BURN/DEX_VAULT`. `STAKE`, `BRIDGE`, `ASSET_ISSUER`, FN and `EXPERIMENTAL` are missing (deferred). Unknown types are correctly invalid. **Existing numbers must not be renumbered.** |
| Policy versions; unknown version invalid | IMPLEMENTED | v1 only; unknown rejected. |
| Canonical commitment preimage per policy | MISSING | Commitment is an opaque `uint256`; canonical per-policy encoding not defined (contract §25). |
| Witness-style transition id separation | IMPLEMENTED | `modern::TransitionId` excludes proofs; `FullTransitionId` commits everything. |
| Authorization binds the full transition | MISSING | Proof verification is **structural only**: `OWNER v1` accepts any non-empty payload and `LEGACY_LOCK v1` checks a publicly known preimage — both forgeable. Safe today only because nothing calls the verifier in production. These two policies are also unconditionally "activated", unlike `BURN`/`DEX_VAULT`. Must be re-gated before any wiring. Tracked as **D6**. |
| Asset registry | MISSING | Deliberately absent; `AssetId` carries no registry state. |
| Deterministic AssetId | PARTIAL | Native issuance id implemented; bridged origin-domain encoding missing (contract §21). |
| Trade by AssetId, never ticker | IMPLEMENTED | Consensus keys on `uint256`; tickers are UI only. |

## 5. Modern era core (contract §17, §28, §54)

| Item | Status | Evidence / gap |
|---|---|---|
| Modern PoS | **MISSING — spec unresolved** | `modern::CheckModernStake` fails closed (`no-modern-pos-rules`). Protocol details are **UNRESOLVED**; see OD-1. **Do not implement until specified.** |
| Modern block validation | MISSING | `modern::BlockValidator` is an interface with no implementation. |
| Modern coinbase/issuance cap | **BLOCKER (latent)** | In the modern era on a legacy-B3 chain there is no coinbase amount cap other than the fail-closed PoS hook. If a validator were installed that did not check issuance, modern blocks could mint arbitrarily. Must be closed as part of OD-1's reward schedule. |
| Modern era still uses stock PoW headers | PARTIAL | `CheckBlockHeader` applies the stock PoW check to marker-modern headers, and `ContextualCheckBlockHeader` uses stock retargeting. Placeholder pending OD-1. The header itself stays Bitcoin-style (contract §17). |
| Modern chain domain (anti-replay) | MISSING | No `ModernChainDomain` constant (contract §63). Does not affect block hashing. |
| Block production (miner/`submitblock`) | MISSING | `Consensus::WithB3BlockCodecV2` has no production callers; `BlockAssembler` is neither legacy- nor modern-B3 aware, and `DecodeHexBlk` hard-codes witness serialization. The node cannot **produce** a valid block on either side of the boundary. |
| Activation-height framework (A1/A2/A3) | MISSING | Contract §54/§55 require deterministic staged activation; none exists. |

## 6. Security and anti-DoS (contract §58–60)

| Item | Status | Evidence / gap |
|---|---|---|
| Cross-boundary reorg handling | IMPLEMENTED | **D1 fixed**, see §1 above. |
| Work/chainwork gating scoped correctly | PARTIAL | **D2 fixed and demonstrated.** Two mechanisms, matching the [fork-choice model](b3-legacy-fork-choice.md): (1) **Header path** -- `AcceptBlockHeader` makes the legacy era **blocks-only** (a legacy-codec header without its block is refused `legacy-header-only`, no index entry, no chain-selection weight; `full_block` flag). (2) **Full-block path (post-X)** -- a full legacy block with attacker-selected chainwork does not need accept-time PoS validation to be neutralized: being off the X-anchored chain makes it **anchor-ineligible**, so it never enters `setBlockIndexCandidates` and never moves the tip, even though `AddToBlockIndex` still credits its `GetBlockProof(nBits)`. This is the trusted-replay *membership* model, not recomputed trust; accept-time legacy PoS trust machinery is deliberately **not** built. Proven by the adversarial case in `legacy_transition_tests` (signed PoS block, absurd `nBits`, bogus kernel, forks below H -> stored, chainwork > tip, marked `BLOCK_ANCHOR_INELIGIBLE`, not a candidate, tip unmoved). Neither imposes PoW on legacy PoS nor constrains `nBits`; the trust formula is unchanged. **Still owed (live-legacy hardening, pre-X only):** port the rolling depth bound (`CheckSync`/`nCheckpointSpan = 500`) and the hardened checkpoint list. These bound deep-fork spam during *live* legacy consensus (height <= H, before X is pinned), where anchor-ineligibility does not yet apply; they must **not** become trusted-replay rejection rules after X. |
| Anchor-ineligible blocks excluded from sync-control state | DONE | **D3 first part.** A fake-chainwork anchor-ineligible block no longer poisons `m_best_header`/best-known-header selection: eligibility is classified once at index creation (`BlockManager::IsAnchorIneligible`, `BLOCK_ANCHOR_INELIGIBLE` set in `AddToBlockIndex`) and filtered at every best-header selection point (`AddToBlockIndex`, `LoadBlockIndex`, `RecalculateBestHeader`, `InvalidateBlock`) and in the candidate set. Recorded `nChainWork` is left intact. Covered by the adversarial transition test (asserts the fake-work block is neither the tip, a candidate, nor the best header). Done first because D3's sync logic depends on trustworthy sync targets. |
| Bounded, progress-safe sync | DONE | **D3.** The legacy sync owner holds a 120s forward-progress lease (`LEGACY_SYNC_PROGRESS_LEASE`), independent of the per-request retry timer, renewed only when a requested block connects to the active chain; on expiry the owner is released (ordinary stall, not scored) and held off by a **per-peer** retry bar (`Peer::m_legacy_sync_retry_after`) so no stalled/silent owner wedges sync and no two unproductive peers ping-pong the window. The empty-inv (and short-page already-known) completion latch is fixed: `m_legacy_sync_exhausted` now means peer-local exhaustion only and releases the window via `NoteLegacyPeerExhausted` unless `LegacySyncTargetReached()` (X on the active chain) is objectively true; a peer's silence can no longer declare sync complete. Orphan-arrival discovery restart is owner-only. `GetDesirableServiceFlags` drops the impossible `NODE_WITNESS` requirement only while the tip is in the **legacy phase** (a lock-free `m_in_legacy_phase` mirror updated on every tip change and seeded from the loaded tip), so a valid legacy peer stays outbound-eligible after its real services are recorded, and the requirement returns once the tip crosses the boundary; non-B3 chains are never in the legacy phase and are unchanged. Covered by `legacy_net_tests` (premature-completion, ping-pong, retry-bar re-poll with a mutation check, outbound eligibility, lease reassignment). Reviewed by an adversarial multi-agent pass; the one confirmed gap (untested reclaim reset) is now closed. |
| Bounded unknown-parent handling | DONE | **D4.** Context-free `CheckBlock` for unknown-parent legacy blocks runs off `cs_main`. A structurally-valid orphan is held in the bounded `node::LegacyBlockOrphanage` (count/bytes/per-peer/expiry) and its children are reprocessed when the parent connects, instead of being re-downloaded; a structurally *invalid* unknown-parent block is scored as misbehavior (objectively malformed). Ordinary out-of-order/timeouts remain unscored. Covered by `legacy_orphanage_tests` and the transition fixture. |
| Structural bounds before cryptography | PARTIAL | Modern `MAX_*` limits are post-decode checks rather than bounded reads (`TransitionProof::payload`, `ModernOutput::policy_params`, vault receipt lists). Harmless while nothing decodes them from a peer; a DoS vector the moment a codec appears. |
| Test hooks excluded from production | DONE | **D7**: both process-global mutable statics are removed. The modern-PoS validator and the asset-policy activation flag are now per-instance `Consensus::Params` fields (`test_only_modern_pos_validator` null, `test_only_asset_policies_active` false), never set by real chainparams; `CheckModernStake` and the proof helpers take them as arguments. Consensus derives from the chain instance's (post-init-immutable) params, not process state — fail-closed in production, thread-safe by construction. |
| Cross-era reorg test matrix | PARTIAL | Most rows of contract §60 are covered by `legacy_boundary_tests`, `legacy_identity_tests` and `legacy_transition_tests`. The D1 fork-point predicate is covered directly; `IsAnchorIneligible` classification (off-anchor side branch ineligible and never a candidate; canonical prefix, X, and modern descendants eligible) is covered in the transition fixture, with the D2 header-only rejection, **and the `BLOCK_ANCHOR_INELIGIBLE` marking path is now exercised** by the adversarial full-block case (a signed PoS block with fake-hard `nBits` and a bogus kernel, chainwork exceeding the tip, marked ineligible and kept out of fork choice with the tip unmoved -- no `FatalError`). |
| Index hash-domain selection | **WRONG** | `src/index/txindex.cpp` and `src/index/txospenderindex.cpp` choose the hash domain from the global chain flag, so a marker-modern block after H yields a scrypt hash present in no block index (contract §7/§67). |

## 7. Deferred subsystems (contract §29–52) — correctly unwired

Per contract §53, none of these may be wired into consensus before a clean H+1.

| Item | Status | Notes |
|---|---|---|
| FlowMesh ledger / clearing / batch | PARTIAL (test-only) | `src/flowmesh/*.h`, header-only, compiled only by unit tests; not referenced by CMake targets or validation. Account-model boundary and no-UTXO settlement are correct. Missing: positions/margin/PnL, deposit identity and idempotence, epoch↔finality binding (OD-6), per-market precision (OD-7), stablecoin fee denomination, fee-asset registry. |
| FlowMesh determinism defects | **BLOCKER (pre-wiring)** | **D5**: arrival-order dependence in batch dedup (credential excluded from the action id) contradicting the file's own determinism contract; equivocation judged before authentication (griefing); an empty-curve UB/crash path; signed overflow in curve evaluation; stale curve reservation that permanently strands funds while the solvency invariant still passes. Must be fixed before any wiring. |
| DEX vault | PARTIAL | Keyless custody, forced change, destination binding and sharding implemented; `finalized_slot` unchecked; **one-time receipt consumption has no consensus implementation** (double-consume possible across transactions in a block); does not call `VerifyTransitionProofs` or check proof type. |
| FN (recognition / license / bond) | MISSING | No modern FN code. Claim derivation blocked on OD-5: only aggregate integration totals are recorded, with no per-owner identity. |
| Bridges, PoW-issued assets, advanced issuance | MISSING | Deferred (OD-8, contract §46–47). |
| Qt / FlowMesh UI | IMPLEMENTED (UI only) | `src/qt/b3*` renders real wallet data or explicit "not available" placeholders; fabricates nothing, calls no FlowMesh or consensus code. |

## 8. Wallet, RPC, networking (contract §56–57, §65–67)

| Item | Status | Evidence / gap |
|---|---|---|
| Modern capability negotiation | PARTIAL | Legacy capability split implemented (`Peer::m_legacy_protocol`, relaxed service bits, protocol caps). `NODE_B3_*` modern capability bits missing. |
| Historical block serving from modern nodes | PARTIAL | Marker-aware serving exists; archival-both-eras story incomplete. |
| Wallet migration / backup compatibility | PARTIAL | Identity preservation makes both possible, and a modern spend of a pre-H UTXO is exercised in `legacy_transition_tests`. Wallet-level support and tests are not built. |
| RPC era field / asset_id | MISSING | `getblockchaininfo` gained `moneysupply`/`fn_integrated`; no `"era"` field and no asset metadata RPC. |
| Indexers preserve historical txids | PARTIAL | Identity is preserved in principle; the index hash-domain defect above must be fixed. |

## 9. Other known defects

| Item | Status | Evidence / gap |
|---|---|---|
| Legacy money supply / FN accounting | PARTIAL | `CBlockIndex::m_legacy_money_supply` / `m_legacy_fn_integrated` are computed and persisted, surfaced via `getblockchaininfo`. FN detection is a **heuristic** (fee ≥ tiered collateral), not a proof of registration; the same heuristic governs consensus fee accounting, so the two at least agree. Both fields stop updating after H (era-gated write), so post-boundary values would read as 0. No direct test coverage. |
| Block index disk format | PARTIAL | Two `int64_t` fields were appended to `CDiskBlockIndex` with no format version bump, for all chain types. Existing databases will not round-trip; a reindex is required and nothing detects or reports this. |
| Stock Core test suites on `ChainType::MAIN` | PARTIAL | ~10 upstream suites fail because MAIN is a legacy PoS chain with `COIN = 1e6` and B3 address prefixes; catalogued in [b3-test-baseline.md](b3-test-baseline.md) and deferred to a scoped identity-isolation task. |

---

## Minimal critical path to a clean H+1

Ordered. Everything not listed is deferred behind later activation heights (contract §53/§54).

**Step 0 — Repository context (this commit).**
Root `CLAUDE.md`, the authoritative contract, this status matrix, and the open-decisions
document, so every later commit has a fixed reference to test against.

**Step 1 — Security blockers.**
D1 (cross-boundary reorg → candidate rejection, never `FatalError`) with the missing
`DisconnectTip` reorg test; D2 (scope the work/chainwork gates **without** imposing PoW on
historical PoS blocks); D3 (bounded, progress-safe sync); D4 (bounded unknown-parent
handling; move `CheckBlock` off `cs_main`); D7 (exclude test hooks from production builds).

**Step 2 — Legacy correctness and equivalence.**
~~Restore the historical checkpoint list~~ (done), ~~close the mempool era-rules gap /
boundary flush~~ (done), ~~source or remove the historical reward-cap exception~~
(done — resolved into three sourced rules, one previously missing), ~~build the
deterministic UTXO commitment tool~~ (done — plus the `b3coin-utxo-verify` operator
command), ~~wire `TrustedReplay` into the node~~ (done as dormant code — pinned-boundary
blocks connect through the engine; admission, read-back and fork choice are
replay-scoped, with off-anchor tip recovery; **inert until H/X are pinned, which is
gated below**), ~~build the three-way equivalence framework~~ (done — reference
exporter on the legacy codebase branch `claude/b3-master-utxo-export`, canonical row
files, `-masterrows=` three-way verdict; see
[b3-utxo-equivalence.md](b3-utxo-equivalence.md)); remaining — **the activation
gate**: run the three-way comparison `U_master == U_port == U_replay` on real B3
history at the candidate (H, X) (operator step: legacy-client export + two synced
captures). H/X must not be pinned and no further replay wiring may be added until it
passes.

**Step 3 — Modern block validation.**
Implement `modern::BlockValidator` (context-free, contextual, and reorg prohibition).
*Modern PoS is blocked on OD-1 and is not part of this step.*

**Step 4 — Legacy→modern spend and block production.**
Wire `ViewLegacyCoin` plus a frozen `FINAL_LEGACY_RULESET` into the modern spend path; make
the miner and `submitblock` marker-aware so the node can produce a modern block; fix the
index hash-domain defect.

**Step 5 — Activation plumbing.**
H/X as mainnet consensus constants with no runtime override (regtest/testnet override
facility per contract §64); the staged activation-height framework for A1/A2/A3.

**Step 6 — Modern PoS.** Blocked on **OD-1**. Once specified: eligibility, kernel,
retarget, reward schedule including the issuance cap, PoS block structure, and block
signature scheme.

Completing Steps 1–5 plus Step 6 yields a produced-and-validated modern H+1 on regtest,
which is the milestone that unlocks the Phase-3 subsystems.

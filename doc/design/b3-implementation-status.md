# B3 Implementation Status / Gap Matrix

Status of the tree against [b3-architecture-contract.md](b3-architecture-contract.md).
Baseline: commit `a8ad010` (the tip of the completed experimental stack, from which
`test/b3-clean-architecture` — formerly `claude/b3-clean-architecture` — is branched).

> **Phase-model note (2026-08-16).** The transition architecture now inserts a
> **temporary-PoW corridor** between legacy PoS and modern PoS
> ([b3-during-fork-transition.md](b3-during-fork-transition.md)): H+1…H+1000
> are modern-format PoW blocks (Policy Outputs active, STAKE outputs created
> and matured); modern PoS begins at M = H+1001. Everything below that says
> "modern" for heights immediately after H refers to the modern *format*;
> the modern *PoS* rows bind at M. The tree and tests now dispatch the three
> phases through `ConsensusPhase` {LEGACY_POS, TRANSITION_POW, MODERN_POS}; the
> former two-phase/H+1-modern-PoS note is obsolete.
>
> **Historical corridor implementation checkpoint (2026-08-16, regtest-complete).** The
> approved staged build-out landed: `ConsensusPhase` dispatch
> (`consensus/era.h`, `transition_pow_length` on params, mainnet 1000);
> corridor validation by scrypt eligibility (`CheckTransitionPowEligibility`,
> constant `transition_pow_bits`, fail-closed `no-transition-pow-rules` when
> unset); phase-aware production (assembler + regtest generate);
> LEGACY_LOCK crossing spends under the frozen legacy rule set with frozen
> legacy maturity; `STAKE = 4` with the owner-ratified v1 script carrier;
> `STAKE_ACTIVATION_DEPTH = 20` (`h − b ≥ 20`); per-validator-key weight
> aggregation (`node/stake_registry`); and the full 1,000-block regtest
> corridor test ending at the fail-closed modern gate at the first
> post-corridor height. At that checkpoint mainnet H = 810,000, corridor bits
> `0x1f008000`, fees-only reward, minimum stake, and the absence of
> cutoff/readiness gates were pinned; X and the seal-derived Modern-PoS values
> were still unset. They are now pinned in the current-status block below. The
> earlier H+1-fail-closed test expectation has moved to H+1001 as the
> corridor design requires. Known follow-up: `HasValidProofOfWork` returns
> true **chain-wide** for any `legacy_b3coin` chain (corridor headers are
> not wrongly checked — the cheap header-spam pre-filter is simply disabled
> for the whole chain, in every era; corridor headers ARE header-only
> verifiable by scrypt and could re-enable it). Post-corridor header rules
> are now the frozen Modern PoS V1 set when configured (see §5).

> **Historical release-v1 ruling checkpoint (2026-08-23).** X-distribution PAUSE
> fails closed: with `hard_fork_height` set and `legacy_final_hash` unset the
> node accepts through H and refuses every header above H
> (`legacy-boundary-unpinned`, no-penalty, nothing marked invalid), block
> production refuses post-H blocks, and startup raises a
> `LEGACY_BOUNDARY_UNPINNED` warning (`Consensus::LegacyBoundaryHeightOnly`).
> Corridor bits must be the CANONICAL compact encoding (`IsCanonicalCompactBits`;
> the ruled mainnet value `0x1f008000` is pinned in chainparams and by test;
> the pin gates then remaining were recorded in
> [b3-release-v1.md](b3-release-v1.md)). Corridor
> pacing VERIFIED
> compressible and then RULED: minimum spacing 60 s + future bound 120 s,
> consensus for every corridor (test `corridor_pacing_enforced`, corridor
> doc §6.1). Validator UX v1: wallet validator key,
> `createstake`, `getstakinginfo`, `startstaking`/`stopstaking`,
> `node::StakingLoop`, STAKE-carrier standardness/ownership/signing
> (`modern::StakeOwnerScript`), STAKE outputs excluded from auto coin
> selection. Seed `176.31.13.198` added. At this historical checkpoint mainnet
> H and corridor bits were pinned while X and the Modern-PoS parameter block
> were still unset; the current pins follow.

> **Mainnet transition pins (2026-09-01; current).** The sealed boundary is
> H = 810,000 with
> X = `2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6`;
> the 1,000-block corridor ends at 811,000 and Modern PoS begins at
> M = 811,001. The sealed spendable supply is
> S_H = 1,042,617,596,101,695,152 base units and the initial Modern-PoS reward
> is R0 = 19,836,712,254 base units. FN Genesis is pinned to 3,592 rows, rights
> root `e8f282a7dcaa9a8fbcfcc5c22ba4f456e5b50968fcf899aaacdaca65bef898ec`,
> and complete-artifact SHA-256
> `c80470eec785600f33fa2e69c520ff331c2b354ebf6e0a9bf8cae7d1eb5f9dca`.
> The post-M schedule is A1 = 812,000, A2 = 813,000, and A3 = 815,000.
> Final-H equivalence and independent byte-identical FN-manifest reproduction
> are complete; these constants are no longer release blockers.

> **FN/asset/FlowMesh activation ruling (through 2026-09-01; current governing target).**
> [b3-fn-assets-activation-design.md](b3-fn-assets-activation-design.md)
> supersedes every proof-carrying or post-M historical-FN activation statement
> below. The transition release pins the full canonical FN manifest, count, and
> Merkle root; block 810,001 coinbase must create one amount-1 FN output per
> manifest row; ordinary 30-block coinbase maturity is the only transfer delay;
> old action types 1 and 2 remain reserved/dead; modern capacity is
> `5,000 - R`; modern PoD and simple-v1 assets activate at separate post-M
> heights A1/A2; asset issuance pays a 1,000 B3 treasury fee; A2 also opens
> FN-seat binding and vault preparation; and A3 activates FlowMesh spot after
> a preparation runway of at least 30 blocks.
> The transition branch now implements the production manifest builder and
> root, exact FN Genesis coinbase validation/production, ordinary owner-script
> authorization, proof-free modern PoD type 6 with branch-local persistent
> counters and non-reclaimable accounting-gap destruction, plus A2 asset
> activation and its 1,000 B3 treasury fee. `getassetstate` reports the
> fail-closed schedule and branch-local FN issuance state. The seal-derived
> pins and independent reproduction are complete. Remaining release gates are
> complete release-level tests/review and the real-history shadow-fork
> rehearsal.

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
| Enter modern only if `height==H && hash==X` | IMPLEMENTED | `src/consensus/boundary.h` `CheckLegacyBoundaryHeader`; dispatched in `ContextualCheckBlockHeader` (`src/validation.cpp`). Mainnet pins H = 810,000 and X = `2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6`. |
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
| Replay == legacy state equivalence test | **PASSED AT FINAL SEALED H (2026-09-01)** — the historical 2026-08-22 campaign at T1 95,350 / T2 110,000 / T3 797,000 was three-way EQUAL with mutation-negative coverage; the final run at H = 810,000 and X = `2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6` also passed | **The invariant is three-way** ([b3-utxo-equivalence.md](b3-utxo-equivalence.md)): `U_master(T) == U_port(T) == U_replay(T)`. The original tool comparison establishes only `U_port == U_replay` — the port's own live-validation chainstate vs the port's own replay, two reimplementations that cannot detect a shared porting divergence from the historical client. The reference side is now in place: branch `claude/b3-master-utxo-export` (on the legacy `master` codebase) adds an `-exportutxo=<file> -exportutxoat=<X>` init mode that walks the old client's own LevelDB transaction index with the old client's own deserialization and emits the canonical logical row file (`b3-utxo-rows/v1`: outpoint, raw amount, exact script bytes, height, coinbase/coinstake flags, tx time, in-block offset; sorted by raw outpoint bytes; byte-identical across producers; never raw DB serialization). `b3coin-utxo-verify` gained `-portrows=`/`-replayrows=` (canonical row export of its two states) and `-masterrows=` (ingest the reference export, verify the full three-way invariant, report per-row differences; exit 0 only when all three agree). Mismatches are diagnosable by canonical rows, not commitment alone. **Historical gate rule (satisfied 2026-09-01): H/X were not to be pinned, and pinned-mode replay was to stay dormant, until the three-way comparison passed on real B3 history at the final (H, X).** The master exporter is **compiled and smoke-verified**: full `b3coind` (export branch, unmodified legacy sources + a documented `makefile.osx-arm64` variant) links against source-built OpenSSL 1.0.2u / BDB 4.8.30.NC / boost 1.63 / vendored LevelDB; offline runs confirm the tip-mismatch refusal and a well-formed zero-row export at the genuine B3 genesis. The export branch also carries `-exportstopatheight` capture tooling (freeze the datadir at exactly T). The audited membership predicate, the verified build recipe, the three mandated capture heights (checkpoint 95350; 110000 covering the repair-window/restricted/superblock region; a recent well-buried height), the bisect procedure and the row-mutation negative test are locked in [b3-utxo-equivalence.md](b3-utxo-equivalence.md); At the time of this audit entry, executing the captures on real history was the remaining operator step; the final-H capture is now complete. Original two-way tool detail: deterministic canonical UTXO-equivalence tool (`src/node/utxo_commitment.{h,cpp}`, non-consensus): `UtxoSetCommitment` folds each outpoint plus the **exact Coin contents** (value, script, height, coinbase/coinstake flags, legacy `nTime`/`nTxOffset`) in canonical outpoint order — not aggregate supply; `CompareUtxoViews`/`CompareUtxoSets` produce a commitment per side and per-outpoint mismatch diagnostics (one-sided or differing). Unit test (`utxo_commitment_tests`) proves it detects every mismatch kind including each Coin field; the transition fixture now proves **full-set** `U == U'` (trusted replay of genesis..H vs the fully-validated live chainstate) by equal commitment with zero mismatches. **Operator command shipped: `b3coin-utxo-verify -datadir=<dir> -height=<H> -hash=<X>`** (`src/bitcoin-utxo-verify.cpp` + core `src/node/utxo_equivalence_check.{h,cpp}`) — against a cleanly-stopped node (synced with `-stopatheight=H`): refuses non-existing databases (never creates or writes into the datadir), loads the block index read-only, walks the X-anchored chain to genesis, reads raw legacy blocks (xor-aware, each re-verified against its indexed marker hash), replays via `TrustedReplay` into a disposable scratch DB, verifies the chainstate best block is exactly X at H, compares the full sets, prints counts, both commitments and bounded per-outpoint diagnostics; exit 0 only on `U == U'`, 1 on any difference or verification failure, 2 on usage/environment errors. Outside consensus and startup by construction. **Final-H completion (2026-09-01):** the operator run passed at H = 810,000 and the pinned X; this gate is closed. |
| Mempool era rules + boundary flush | DONE | Admission is defined by the **era of the next block** (`active_tip_height + 1`) at the single choke point every path funnels through (`MemPoolAccept::PreChecks`): pre-H only legacy-encoded transactions, validated under legacy next-block rules (`CheckLegacyTxInputs` — coinbase+coinstake maturity, input-time rule, proof-of-integration fees; standalone coinstake refused as in the historical client; consensus script caching selects the next block's flags, so a modern tx at tip==H is never cached under witness-free legacy flags); post-H only modern-encoded. Provenance = decode context (per-peer wire codec / block codec / local construction), never reinterpreted; no global codec switch, no family byte. Connecting H **atomically empties the pool** under `cs_main` + the mempool lock. `mempool.dat` format 3 carries per-tx codec provenance, so identity survives a dump/load and a pre-H file can never repopulate a post-H node (the gate refuses each entry on reload; formats 1/2 remain readable). Reorg resurrection passes through the same gate; the reorg maturity filter applies coinstake maturity in the legacy era; a reorg across H cannot resurrect legacy txs because it cannot happen (D1). Pre-H RPC/wallet submissions decode modern and are cleanly refused — legacy submission over RPC is explicit follow-up work. Boundary-tested H-1→H→H+1 in the transition fixture, incl. persisted entries and within-era resurrection. |
| Historical reward-cap exception | DONE | Provenance investigation resolved the flat exception into **three sourced rules**, now ported faithfully (see [b3-legacy-fork-choice.md](b3-legacy-fork-choice.md) "Historical reward-rule exceptions"): (1) the repair window 77447–77505 (verbatim in the final client, kept as `legacy::IsRepairWindowHeight`); (2) height 107488 was **not** a cap bypass but a structured superblock rule — last coinstake output ≤ 75,656,908 × 1e9 units to the pinned P2PKH payee — the former blanket bypass dropped that validation, now enforced (`bad-cs-superblock`, params `legacy_superblock_height/pubkey`); (3) a previously **missing** unconditional rule: above height 78000 a coinstake whose second output pays the restricted destination (`ShJsVNBQ…`, hash160 `db8c…b0a5`) with positive reward is rejected (`bad-cs-restricted`), using the 0.8-era ExtractDestination fold (P2PK→key-hash) that modern ExtractDestination lacks. The FN-payment check is deliberately not ported (gated on `!IsInitialBlockDownload()` in the final client — not a sync-consensus rule — and barred by the no-FN scope decision). Trusted replay never adjudicates rewards, so it is unaffected. Golden-vector unit tests in `legacy_pos_tests`. |
| Frozen legacy script ruleset | IMPLEMENTED (naming differs) | The frozen set exists as `LEGACY_BLOCK_SCRIPT_FLAGS` (validation.cpp), and since commit `45ba2ec` the modern spend path DOES select frozen legacy flags for pre-H coins (`LegacyLockSpendContext`, wired at block connect and both mempool script checks, committed into the script-cache key). Only the contract-§16 `FINAL_LEGACY_RULESET` *name* and the declarative `ViewLegacyCoin` wiring remain. (This row was stale.) |

## 4. Legacy-spend bridge and modern model (contract §15, 19–27)

| Item | Status | Evidence / gap |
|---|---|---|
| `LEGACY_LOCK` view of a legacy prevout | IMPLEMENTED | `modern::ViewLegacyCoin` is the required non-mutating projection; the modern spend path selects frozen legacy script semantics for authenticated pre-H coins at block connect and both mempool script checks. |
| Native B3 reserved asset id | IMPLEMENTED | `modern::NativeAsset()` (all-zero), never issued via the generic engine. |
| Modern output model | IMPLEMENTED | `modern::ModernOutput` matches the contract, plus the ratified `policy_params` field. |
| Policy type coverage | PARTIAL | `LEGACY_LOCK=0, OWNER=1, BURN=2, DEX_VAULT=3, STAKE=4, FN=5` exist (the earlier "STAKE/FN missing" wording was stale). `BRIDGE`, `ASSET_ISSUER` and `EXPERIMENTAL` remain absent (deferred; asset issuance is a creation ACTION, not a policy type). Unknown types are correctly invalid. **Existing numbers must not be renumbered.** |
| Policy versions; unknown version invalid | IMPLEMENTED | v1 only; unknown rejected. |
| Canonical commitment preimage per policy | MISSING | Commitment is an opaque `uint256`; canonical per-policy encoding not defined (contract §25). |
| Witness-style transition id separation | IMPLEMENTED | `modern::TransitionId` excludes proofs; `FullTransitionId` commits everything. |
| B3A1 owner authorization | IMPLEMENTED | Contextual validation recognizes a canonical B3A1 carrier only with authenticated post-H Coin provenance, then evaluates its carried owner suffix with the ordinary P2PKH, P2SH, witness, taproot, or multisig rules. Signing follows the same provenance rule and signs the modern transaction normally. The structurally weak historical type-1/type-2 `TransitionProof` helpers are dead and never authorize FN or asset creation. |
| Asset registry | MISSING by design (v1) | No consensus registry: with `mint_authority = NONE` the genesis mints the whole supply once and conservation forbids any later surplus, so the cap holds by construction. Wallets/DEX read an asset's rules from its issuance transaction via a derived (non-consensus) index — follow-up engineering. |
| Deterministic AssetId / simple-v1 genesis | **IMPLEMENTED; A2 PINNED, release verification pending** | `modern::AssetIdV1`, immutable `AssetGenesisV1`, B3A1 output parsing, exact fixed-supply genesis, conservation, and the no-remint rule are enforced by the production mempool/block path. `asset_activation_height` is a real post-M consensus parameter, both A1/A2 pins are required for a valid schedule, pre-A2 issuance fails closed, and genesis must pay 1,000 B3 to the configured treasury. Native fees remain separate. `issueasset`, `sendasset`, and `burnasset` construct the wallet transactions; burns are B3A1 `PolicyType::BURN` outputs, never `OP_RETURN` carriers. Mainnet A2 = 813,000 is pinned; release verification remains. |
| Trade by AssetId, never ticker | IMPLEMENTED | Consensus keys on `uint256`; tickers are UI only. |

## 5. Modern era core (contract §17, §28, §54)

| Item | Status | Evidence / gap |
|---|---|---|
| Modern PoS | **IMPLEMENTED; MAINNET PINNED (frozen V1)** | The owner-frozen V1 rule set ([b3-modern-pos-spec.md](b3-modern-pos-spec.md)) is live behind `Consensus::Params::modern_pos`: seed-chained deterministic stake-weighted eligibility (`modern/pos_v1.h`), exact round timestamps, sentinel `nBits`/zero `nNonce` (no retarget — `w/W` normalization is the difficulty), BIP340 validator block signatures (trailing `vchBlockSig`, outside identity), height→round→hash PoS-native fork choice (params-aware candidate comparator), reorganization horizon D (no-penalty refusal), incremental `node::StakeTracker`, per-index persisted eligibility digests, and deterministic production + signing in the assembler. **Mainnet pins the complete parameter block: Modern PoS begins at M = 811,001 with sealed S_H = 1,042,617,596,101,695,152 base units and R0 = 19,836,712,254 base units.** Covered by `modern_pos_tests` (8 cases: guard, params sanity, normal operation, low-online-stake recovery incl. fork choice + horizon, invalid signature/eligibility/reward, restart+reindex). The X/R0/`ModernPosParams` and final-H equivalence gates are complete; release review and rehearsal remain. |
| Modern block validation | PARTIAL | The V1 header/connect rules above ARE modern-PoS validation; the `modern::BlockValidator` interface skeleton remains unimplemented/unwired (its reorg contract is enforced by `Consensus::ReorgFromForkCrossesLegacyBoundary` + the horizon instead). |
| Modern coinbase/issuance cap | **DONE (unconditional)** | `ConnectBlock`'s modern-PoS branch enforces `coinbase ≤ fees + modern_reward` OUTSIDE the validator dispatch (fees-only when unconfigured — nothing mints by omission) and rejects any coinbase output claiming the STAKE magic (`bad-cb-stake`, ruling M6). The reward schedule itself stays an owner parameter (OD-2). |
| Modern era still uses stock PoW headers | RESOLVED | With `modern_pos` set, marker-modern PoS headers are judged by the V1 rules (sentinel bits, exact time, horizon) and never by stock PoW/retarget; with it unset the stock placeholder remains (non-mainnet networks may remain unset; mainnet is configured). Load-time and context-free header paths defer accordingly. |
| Modern chain domain (anti-replay) | IMPLEMENTED | `modern::ModernChainDomain` (`modern/fn.h`), fail-closed on an unpinned boundary; the domain for FN identity and now the modern-PoS seed/eligibility/signature tags. (This row was stale — the constant landed 2026-08-17.) |
| Block production (miner/`submitblock`) | PARTIAL | `BlockAssembler` is fully phase-aware: refuses legacy-era production, produces corridor templates, and produces deterministic signed modern-PoS blocks (`modern_pos_validator_key` + `SignModernPosBlock`). `submitblock` and GBT proposal decoding are chain-aware: B3's explicit header marker selects `TX_MODERN`/MPA while unmarked historical blocks retain the exact legacy codec. Remaining: RPC/wallet production wiring. |
| FN/asset/FlowMesh activation framework | **IMPLEMENTED; MAINNET PINS COMPLETE** | The production schedule enforces the mandatory 810,001 genesis and ordered post-M gates A1/A2/A3, including `A3 >= A2 + 30`. A2 permits FN-seat binding and vault preparation; A3 permits FlowMesh spot trading and vault effects. Missing or contradictory pins fail the gated feature closed. Mainnet pins M = 811,001, A1 = 812,000, A2 = 813,000, and A3 = 815,000, together with the 3,592-row FN manifest and its rights root. Release review and rehearsal remain. |

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
| Index hash-domain selection / serialization | **DONE** | `txindex` and `txospenderindex` derive block identity with marker-aware `GetMarkerHash` and select legacy or `TX_MODERN` transaction serialization from the actual block codec. Modern witness/MPA data and historical txids therefore survive index write/read without a global-era hash assumption. |

## 7. FN, assets, FlowMesh, and bridge (contract §29–52)

The current transition-release target includes FN Genesis, modern PoD,
simple-v1 assets, and working FlowMesh spot. A2 enables FN-seat binding and
vault preparation; A3 enables trading and vault effects after at least 30
blocks. Each market's epoch-0 anchor is the earliest canonical block at or
after `market.created_height` whose post-block FN-v2 seat set has at least four
members; sequence zero waits until that exact anchor is 30 blocks deep.
Bridge-backed bUSD minting has a separate fail-closed readiness gate and is not
made safe merely by reaching A3.

The pinned mainnet schedule is M = 811,001, A1 = 812,000, A2 = 813,000,
and A3 = 815,000. FN Genesis at 810,001 contains 3,592 historical rights.

**Current implementation summary (2026-09-01):**

| Item | Status | Current state |
|---|---|---|
| Mainnet seal, FN Genesis, and feature schedule | **PINNED** | H = 810,000; X = `2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6`; M = 811,001; S_H = 1,042,617,596,101,695,152 base units; R0 = 19,836,712,254 base units; FN count = 3,592; rights root = `e8f282a7dcaa9a8fbcfcc5c22ba4f456e5b50968fcf899aaacdaca65bef898ec`; artifact SHA-256 = `c80470eec785600f33fa2e69c520ff331c2b354ebf6e0a9bf8cae7d1eb5f9dca`; A1/A2/A3 = 812,000/813,000/815,000. |
| FlowMesh production path | **IMPLEMENTED; mainnet schedule pinned, rehearsal pending** | Spot ledger, deterministic clearing, authenticated certified-log runtime, P2P/service lifecycle, production store, checkpoint/vault plumbing, wallet/RPC surfaces, and A2/A3 gates are wired. A later release is expansion only. |
| DEX vault and withdrawals | **IMPLEMENTED; mainnet schedule pinned, rehearsal pending** | Modern B3A1/MPA policy paths use no `OP_RETURN`; keyless vault custody, receipt nullifiers, forced change, and destination binding are enforced. Pending obligations are capped by deterministic top-64 live pool-UTXO capacity; payout chooses amount descending then outpoint ascending. The publisher sends one withdrawal, waits for confirmation, refreshes state/capacity, rebuilds, then sends the next. |
| FlowMesh treasury settlement | **IMPLEMENTED; release verification pending** | After each ordinary slot, the deterministic maximal partial flush is `min(accrued treasury available, anchored native capacity - existing pending native withdrawals)` when positive. It never waits for the full balance; zero capacity never blocks trading. |
| Bridge-backed bUSD | **DECENTRALIZED PATH IMPLEMENTED; MAINNET FAIL-CLOSED PENDING LATER-BUILD PINS/AUDIT/SAFETY** | Canonical Ethereum-mainnet USDT (6 decimals) targets the new immutable `B3StakerBridge` keyless vault, not the historical managed smoke vault. B3's bounded type-10 path verifies the Ethereum light client and exact receipt/log and contains mint/burn/nullifier/cap machinery, but implementation is not activation. C++/Solidity vectors cover bootstrap, certificates, gas, withdrawals, and AssetId byte order. The exact audited verifier/vault may be deployed before M; then a later B3 build must pin the complete tuple. Missing or mismatching fields fail closed. Inbound B may be enabled after readiness while outbound W remains unset; that permits proven deposit mints but creates a disclosed custodial waiting period because every withdrawal record remains invalid. W requires a later round-trip safety review. FlowMesh A3 activates neither gate. |

The detailed table below is a **historical implementation snapshot through
2026-08-31 and is superseded by the current summary above**. Its present-tense
phrasing must not be read as current release state.

**FN economics update (owner rulings through 2026-08-31):**
`RequiredDisintegration` pins 15,000 / 30,000 / 60,000 B3 price tiers.
The measured 3,500 historical count is a height-807,709 floor; final modern
capacity is exactly `5,000 - R`, where R is the final pinned manifest count.
Production modern-PoD validation uses those exact tiers and excludes the
required destruction from producer-claimable fees.

| Item | Status | Notes |
|---|---|---|
| FlowMesh ledger / clearing / batch | PARTIAL (compiled, activation-unwired) | `src/flowmesh/*.h`, header-only, compiled only by unit tests; not referenced by CMake targets or validation. Account-model boundary and no-UTXO settlement are correct. Missing: positions/margin/PnL, deposit identity and idempotence, epoch↔finality binding (OD-6), per-market precision (OD-7), stablecoin fee denomination, fee-asset registry. |
| FlowMesh determinism defects | **FIXED (2026-08-19, pass 3 verified; compiled, activation-unwired)** | **D5 all fixed with regression tests** (`flowmesh_batch_tests`, `flowmesh_clearing_tests`): (1) batch dedup is credential-aware — one action id with several credentials authenticates iff ANY does, arrival-order independent (pinned by permuted-input root equality); (2) authentication now precedes equivocation grouping, so a forged action cannot manufacture an equivocation against an honest signer; (3) curve evaluation is a total function (empty/garbage input degrades deterministically, never UB); (4) interpolation is exact in 128-bit where the 64-bit product overflowed; (5) settlement decrements each curve's recorded reservation by exactly what it consumed, so cancel/exhaustion release the exact remainder — nothing strands invisibly. **(6) NEW, found in adversarial self-review beyond the catalogue:** the bid worst-case reservation was a per-segment max-rectangle, but a PERSISTENT curve filled across slots at descending prices can spend up to the staircase SUM `Σ (q_i − q_{i+1})·price_{i+1}` — exceeding the old bound and silently breaking settlement (also protecting the settlement `*Quote` dereference). Reservation corrected to the staircase bound; the adversarial multi-slot sequence (spend 601 > old bound 600) is a pinned test. Pass 2 (owner corrections, verified): SubmitCurve atomic via reservation-delta accounting (no ghost curves; failed first submission/replacement leaves state root byte-identical); zero-demand curves rejected (no free book entries/candidate prices); EvaluateCurve genuinely total (MoneyRange-validated points, 128-bit-widened subtraction, INT64_MIN/MAX safe, UBSan-clean on the real header); no silent masking (CancelCurve keeps the curve on release failure, settlement preflights quotes and per-curve reservation sufficiency, every ledger move checked, exact subtraction — impossible states assert visibly); credential variants canonicalized (sorted, deduplicated, counting-authenticator test pins identical call order across arrival permutations); book commitment canonically framed (v2 domain, curve+breakpoint counts, pinned empty-book vector, layout-mutation distinctness test). Pass 3 (final review corrections, verified): (a) ZERO-PRICE SETTLEMENT — a zero uniform clearing price is a valid outcome (curve validity deliberately permits price-0 breakpoints; banning them would be a market-economics decision, not a bug fix). Previously the zero-amount quote move was rejected by the ledger after the base move succeeded and the engine asserted only after partial mutation. Now the quote leg is an explicit successful no-op while the base leg settles in full; ledger source reservations are preflighted per side/account BEFORE the first settlement mutation; the first leg is checked before the second leg executes; and the invariant asserts are documented honestly as fail-fast aborts, not transactional recovery. Pinned test: 10 lots clear at price 0, zero quote moves, 10 base moves seller→buyer, both curves/reservations unwind exactly, solvency holds, no assertion/partial-failure path (also UBSan-verified). (b) LEDGER ROOT CANONICALLY FRAMED — `Ledger::StateRoot` domain bumped `b3/flowmesh/state/v1` → `v2`; balance, custody and pending-receipt counts now precede each variable-length collection. Empty-ledger v2 root pinned; the framed preimage is reconstructed byte-exactly in a test (and shown NOT to match without the counts); the clearing empty-book root was repinned over the v2 ledger root, so the full engine commitment is framed end to end. (c) The curve-framing test was repaired to genuinely isolate the framing claim: both compared engines hold byte-identical ledger state (equal ledger roots asserted) with the same flattened breakpoint stream split differently across curves, plus a byte-exact framed-preimage reconstruction. Layer remains header-only/test-only and unwired into consensus. |
| FlowMesh certified-log layer (microblocks) | PARTIAL (compiled, activation-unwired; **owner-accepted direction 2026-08-19; Codex repair pass applied same day**) | The accepted certified-deterministic-execution-log architecture is implemented and tested (commits `bd80478`, `03ef8cb`, `6532f52`, `cc9b10b`, `a23244c`, `49a0852`; decision register [b3-flowmesh-dex-decisions.md](b3-flowmesh-dex-decisions.md)): copyable `FlowMeshState` with a PURE canonical root + separate execution-result commitment (MB-0 atomic candidate execution); `MicroblockCoreV1` with certificate-separate identity; BIP340 attestation certificates with the fault-model threshold relation explicit (`MinCertificateThreshold`, 2t−k>f / t≤k−f); minimal round/lock leader recovery behind interfaces (round-robin provisional); DEPOSIT actions carrying only an outpoint judged by a `DepositVerifier` at an explicit `AnchorRef` (fail-closed; production verifier deliberately unavailable until vault activation); BUY/SELL limit intents as degenerate curves on the unchanged clearing economics; bounded action pool; transport-agnostic `MeshNode` (propose/re-execute/attest/certify/commit, catch-up, equivocation evidence); durable `FlowMeshStore` log (atomic entry+marker appends, re-verifying replay) with certificate-authenticated snapshots; `ChainAnchorPolicy` (OD-6 mechanics, depth = owner input); Schnorr action credentials; fuzz targets for all codecs. B3 gate green (29 suites; 212 cases MEASURED after the four-blocker closure, commit e9a52e4: reachable BID residual band, anchors rechecked after the durable lock write and before proposal signing, store-neutral startup, futures text de-normativized; atop the follow-up pass 178e155: non-copyable signing nodes, executor-only raw state transitions, complete strict store namespace validation with a hard [1,4096] journal bound, exact ask residuals and exhausted-curve rejection at snapshot decode, committed-anchor recheck before execution/cache, marker-free invalid startup; atop the self-audit pass 66a2d3b: vault-checked snapshot decode, serialized store appends + one-validator-per-store, full beyond-tip probe, validated anchor depth, base!=quote guard, de-vacuized Byzantine tests) incl. three-node convergence, restart via the gated StartValidator lifecycle, snapshot full-prefix authentication (chain+certificates+evidence+anchors) with corruption fallback, catch-up with anchor revalidation, vote-split recovery, the k=4/f=1/t=3 split-lock SAFE-non-finalization case (cross-round unlocking: OWNER DECISION), equivocation containment, anchors rechecked immediately before signing and commit, market/execution-config-bound authorization (cross-market replay dead), observer-only public construction (signing only via node::StartValidator), serialized compare-and-set lock journal, store error-vs-missing discipline and fresh-namespace detection, outage isolation. Root cost measured (128 µs @1k / 1.3 ms @10k accounts) — incremental commitment deliberately deferred. Remaining engineering: net_processing/RPC wiring (parked until the FN seat lifecycle exists), production deposit/withdrawal verifiers (base-chain gated). Open owner decisions listed in the register §3. Compiled into the node library; wired into no consensus/networking/RPC path; B3 consensus untouched. Codex repair passes (three, 2026-08-19/20): canonical proposer- AND credential-free semantic microblock identity with admission evidence outside identity, assert-free fallible clearing/settlement, insert-free ledger rejections, exact integer-price bid reservation, ledger-binding removed from the engine (methods take the owning ledger), write-ahead durable lock journal + persist-before-live commit ordering with fail-stop halts, anchor canonicality revalidation of certified history/replay/snapshots, withdrawal lifecycle renamed to REQUESTED (no redeemable view exists), bounded strict decoders throughout, quorum shape validation. |
| DEX vault | PARTIAL | Keyless custody, forced change, destination binding and sharding implemented; `finalized_slot` unchecked; **one-time receipt consumption has no consensus implementation** (double-consume possible across transactions in a block); does not call `VerifyTransitionProofs` or check proof type. |
| FN — historical 2026-08-31 activation target | **IMPLEMENTED; pins/rehearsal were pending at this snapshot** | The worktree contained the canonical `{pod_id, recipient_key_hash}` manifest/export, raw-PoD sorting, chain/height/version/count-bound root, exact 810,001 coinbase event, B3A1 owner/FN outputs, ordinary script authorization, conservation, separate A1/A2 gates, and type-6 modern PoD. The modern count was cumulative per branch, persisted in a backward-compatible block-index sidecar, advanced in transaction order, and used for the exact `5,000 - R` cap and 15k/30k/60k tiers. Mempool/miner accounting removed disintegration from fees; reindex/restart paths restored the branch-local count, and `getassetstate` exposed the configured schedule and next slot/tier. At this historical snapshot, independent final-H reproduction, X/R0/manifest/A1/A2 pins, full review, and shadow-fork rehearsal remained. The old proof-fit gate and `issued[pod_id]` state were not work items. |
| Bridges, PoW-issued assets, advanced issuance | MISSING | Deferred (OD-8, contract §46–47). |
| Qt / FlowMesh UI | IMPLEMENTED (UI only) | `src/qt/b3*` renders real wallet data or explicit "not available" placeholders; fabricates nothing, calls no FlowMesh or consensus code. |

## 8. Wallet, RPC, networking (contract §56–57, §65–67)

| Item | Status | Evidence / gap |
|---|---|---|
| Modern capability negotiation | IMPLEMENTED | Legacy capability split and modern feature advertisement, including `NODE_B3_FLOWMESH`, are wired. Capability bits describe peer support only; they never override activation or validation gates. |
| Historical block serving from modern nodes | PARTIAL | Marker-aware serving exists; archival-both-eras story incomplete. |
| Wallet migration / backup compatibility | PARTIAL | Historical tx identity is preserved. Trusted post-H provenance lets both descriptor and legacy/imported-key wallets recognize and sign the B3A1 owner suffix, while pre-H lookalikes retain their old full-script semantics; focused wallet tests cover the imported-key case. `getwalletassets` inventories exact integer balances/UTXOs, and the write RPCs use native B3 funding/change. Complete backup/migration functional coverage remains. |
| RPC activation / asset state | **IMPLEMENTED; release verification pending** | `getassetstate` reports next-block FN/asset configuration, heights, FN id/count/capacity/next tier, fee, and treasury. `getwalletassets` reports wallet asset balances and UTXOs. `issueasset`, `sendasset`, `burnasset`, and `createfncoin` construct, fund, sign, and optionally broadcast the four production actions. |
| Indexers preserve historical txids and modern payloads | DONE | Marker-aware block identity plus codec-aware legacy/witness/`TX_MODERN` transaction serialization are wired in both `txindex` and `txospenderindex`. |

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
command), ~~wire `TrustedReplay` into the node~~ (done — pinned-boundary blocks
connect through the engine; admission, read-back and fork choice are
replay-scoped, with off-anchor tip recovery), ~~build the three-way equivalence
framework~~ (done — reference
exporter on the legacy codebase branch `claude/b3-master-utxo-export`, canonical row
files, `-masterrows=` three-way verdict; see
[b3-utxo-equivalence.md](b3-utxo-equivalence.md)); ~~run the final activation
gate~~ (done 2026-09-01 — `U_master == U_port == U_replay` at H = 810,000,
X = `2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6`).
The historical rule forbidding H/X pinning before that pass was honored; H/X
are now pinned.

**Step 3 — Modern block validation.**
**DONE through the production validation paths.** The original
`modern::BlockValidator` interface skeleton is not the dispatch point; the
context-free, contextual, boundary, corridor, Modern-PoS, and reorg rules are
enforced by the phase-aware validation code described above.

**Step 4 — Legacy→modern spend and block production.**
**CODE COMPLETE; RELEASE REHEARSAL PENDING.** The frozen legacy spend context, phase-aware miner, marker-aware
block identity, codec-aware index serialization, and chain-aware
`DecodeHexBlk`/submission path are wired. Raw B3 block provenance comes from
the explicit header marker; the decoder never guesses from height or payload.

**Step 5 — Activation plumbing.**
**CODE COMPLETE; MAINNET PINS COMPLETE.** H = 810,000 and X =
`2413ba59476afb9a01b971c350b2c5a51494b37925055be42dde774f30d865c6`;
the mandatory 810,001 genesis coinbase uses the 3,592-row manifest with rights
root `e8f282a7dcaa9a8fbcfcc5c22ba4f456e5b50968fcf899aaacdaca65bef898ec`
and artifact SHA-256
`c80470eec785600f33fa2e69c520ff331c2b354ebf6e0a9bf8cae7d1eb5f9dca`.
The ordered post-M gates are pinned at A1 = 812,000 for modern PoD,
A2 = 813,000 for assets/seat binding/vault preparation, and A3 = 815,000 for
FlowMesh spot.

**Step 6 — Modern PoS.** ~~Blocked on OD-1~~ **DONE as the frozen V1**
(2026-08-21): eligibility, seed chain, exact-timestamp rounds, the
unconditional issuance cap, the coinbase key declaration + trailing BIP340
block signature, PoS-native fork choice with the horizon, tracker, and
deterministic production — implemented, regtest-exercised end to end
(`modern_pos_tests`). Mainnet now pins the complete parameter block and begins
Modern PoS at M = 811,001, based on sealed
S_H = 1,042,617,596,101,695,152 base units and
R0 = 19,836,712,254 base units. The §9 numbers, horizon D, and STAKE carrier
are ratified. Current verification counts are recorded in the release runbook
and CI rather than frozen in this status document.

The transition/FN/asset/FlowMesh paths produce and validate all three phases
on regtest, including FN Genesis and the later A1/A2/A3 feature gates. The
final-H operator gate and all transition/FN/activation pins are complete.
Before the tag, finish full release verification (including the wallet RPC
surface) and the shadow-fork rehearsal.

---

## Addendum — BLS finality implementation batch (2026-08-23/24)

The Modern-PoS V1 finality gadget (ruling M7; normative
[b3-cross-chain-finality-v1.md](b3-cross-chain-finality-v1.md)) is now
**implemented end to end** on branch `test/b3-clean-architecture`
(plan commits 1–18 of
[b3-modern-pos-v1-implementation-plan.md](b3-modern-pos-v1-implementation-plan.md);
P2P `finsig` was folded into commit 15, block assembly + staking into 16,
wallet/RPC into 17, activation plumbing into 18, and the persisted pin was
added as release-blocker 14A):

| Area | Status | Where |
|---|---|---|
| Constants/types/codecs, keccak, blst wrapper | DONE | `modern/finality_types.h`, `crypto/{bls,keccak256}` |
| Metadata cells (6/7/8), UTXO exclusion | DONE | `modern/metadata_cell.h`, coins/validation |
| FINALITY_KEY binding + derived index | DONE | `modern/finality_key.h`, `node/finality_binding_index` |
| MPA (flag 0x02, registry, costs, ×4 weight), `ptxid` | DONE | `primitives/transaction`, `modern/{mpa,payload_cost}.h` |
| `MODERN_PAYLOAD_ROOT` (Path B, BE leaf index) | DONE | `modern/payload_root.h`, miner |
| Validator-set snapshot (keccak tree, quorum) | DONE | `node/validator_set` |
| Certificate verification + schedule/window rules | DONE | `modern/{finality_certificate,finality_schedule}.h` |
| Epoch state machine (gated rotation, extension, carry-over, lineage break) | DONE | `node/finality_tracker` |
| Finality pin (anchor semantics, header/candidate/InvalidateBlock refusals) | DONE | `node/blockstorage`, `validation` |
| **Persisted pin** (atomic file, monotone, survives restart/reindex) | DONE | `node/finality_pin`, `<blocksdir>/finality_pin.dat` |
| One stake universe (W_block == W_finality, binding required from F=M) | DONE | `Chainstate::ModernEligibilityWeights` |
| Signer / leaderless aggregator / `finsig` gossip (liveness only) | DONE | `node/finality_signature`, `net_processing` |
| Certificate-bearing block assembly + staking-loop signing | DONE | `node/miner`, `node/staking` |
| Wallet BLS key derivation, bind/rotate/revoke RPCs, diagnostics | DONE | `wallet`, `wallet/rpc/staking`, `rpc/blockchain` |
| Binding-tx relay path (cell standardness, dust exemption, TX_MODERN wire, mempool pre-check) | DONE | `script/solver`, `policy`, `net_processing`, `validation` |
| **F = M activation** (`Consensus::ModernObjectRulesActive`: H + X + Modern-PoS rule set) | DONE | `consensus/era.h`; fail-closed behavior when the rule set is unset remains guard-tested by `finality_activation_tests`, while mainnet is now pinned and configured at M = 811,001. |

At the time of this 2026-08-23/24 batch, the open release list was final
X/S_H/R0, the final-H three-way equivalence gate, independent byte-identical FN
manifest/count/root production, exact owner-pinned A1/A2/A3 heights, the
real-history shadow-fork rehearsal, and release signing/CI. This historical
list is retained as an audit record. As of 2026-09-01, X/S_H/R0, final-H
equivalence, the independently reproduced 3,592-row FN manifest/root/artifact,
and A1/A2/A3 are pinned and closed; the shadow-fork rehearsal and release
signing/CI remain. The old proof-carrier size measurement is no longer an
activation gate. Canonical unit baseline after commit 18: see the batch report
(204 registered suites; the same 19 known stock-vector/fixture failures as the
pre-batch baseline).

# B3 Implementation Status / Gap Matrix

Status of the tree against [b3-architecture-contract.md](b3-architecture-contract.md).
Baseline: commit `a8ad010` (the tip of the completed experimental stack, from which
`claude/b3-clean-architecture` is branched).

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
| Replay pipeline | PARTIAL | `legacy::TrustedReplay` / `PersistentReplay` implement parse/linkage/checkpoint/Merkle/apply with the correct skip list, atomically and crash-safely. **Zero production callers** — no init wiring, no RPC. |
| Historical checkpoint anchors | MISSING | Contract §11 step 5 verifies nothing: no checkpoint list exists in chainparams and nothing populates the replay checkpoint map. The `master` client enforced 13 checkpoints; this is also a live-era rule regression. |
| Replay == legacy state equivalence test | MISSING | `src/test/legacy_transition_tests.cpp` cross-checks replay against live chainstate on **synthetic** chains only. No deterministic UTXO commitment tool and no comparison against real `master` state at a real H. Contract §12/§61 make this mandatory before activation. |
| Mempool uses legacy rules in the legacy era | **WRONG** | The mempool path runs stock `CheckTxInputs`/`IsStandardTx` with no era branch: coinstake maturity is not enforced, the `coin.nTime > tx.nTime` rule and FN-collateral fee accounting are absent. Mempool acceptance can disagree with block connection. Documented boundary mempool flush is also unimplemented. |
| Historical reward-cap exception | PARTIAL | `legacy::IsHistoricalStakeRewardCapException` exempts heights 77446–77506 and 107488 with **no recorded provenance** in the tree. Must be sourced or removed — an unjustified reward-cap bypass can bless invalid history. |
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
| Bounded, progress-safe sync | **BLOCKER** | **D3**: legacy sync ownership can be claimed by an unauthenticated inbound peer, which can then wedge sync permanently (empty-inv completion latch, or never replying) while other peers' block announcements are discarded. No stall timeout, no eviction, no misbehavior scoring on legacy paths. A specific ownership policy is *not* locked; the requirement is bounded and progress-safe (contract §59). Sync-target state is now trustworthy (row above). |
| Bounded unknown-parent handling | **BLOCKER** | **D4**: full `CheckBlock` runs while holding `cs_main` for attacker-chosen blocks up to the 5 MB legacy message limit; no orphan cache; out-of-order handling disconnects without scoring. |
| Structural bounds before cryptography | PARTIAL | Modern `MAX_*` limits are post-decode checks rather than bounded reads (`TransitionProof::payload`, `ModernOutput::policy_params`, vault receipt lists). Harmless while nothing decodes them from a peer; a DoS vector the moment a codec appears. |
| Test hooks excluded from production | **BLOCKER** | **D7**: `SetModernPosValidatorForTesting` and `SetAssetPoliciesActiveForTesting` are mutable, non-thread-safe function-local statics compiled into the production binary and reachable from `ConnectBlock`. Consensus validity must not depend on process state (contract §55). |
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
Restore the historical checkpoint list and wire it into both live validation and replay;
close the mempool era-rules gap and implement the boundary flush; source or remove the
historical reward-cap exception; build the deterministic UTXO commitment tool and the
`U == U'` equivalence test against real `master` state; wire `TrustedReplay` into a real
node startup mode.

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

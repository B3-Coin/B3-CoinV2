# FlowMesh V2 clean core: architecture of the `rebirth` outline

Date: 2026-10-16. Branch: `rebirth`, based on `18051472`.
Status: **OUTLINE IN PROGRESS.** Implemented in `src/flowmesh2/`: C1
(`BoundedMailbox`, `ReadSlot`), slice **R1** (build gate, error model, code
registries, Boost.Test runner, inventory and TODO lint), slice **R2** (seven
guards with self-tests, compile-fail controls, evidence tool, sanitizer
canaries, CI workflow, and the durability token type), slice **R3** (core
primitives: checked amounts, ids, tag registry, exact codec, the provisional
profile, authority types, size-bound Merkle trees, markets and the slot
header), slice **R4** (signed actions, slot bodies, outcomes, chain facts,
certificates, seat sets, the account leaf and the shared nonce and fee spec
functions), slice **R5** (the shared spot ledger with nonce windows,
fee withholding and conservation, the protocol accounts, and state
commitment v0 with account proofs), slice **R6** (the order book, the A2
sweep with pairing, fees and fill caps, residual-cross detection and a
naive reference matcher) and slice **R7** (the canonical slot order, the
slot validity predicate, the engine state with its commitment and
slot-boundary invariants, and `Engine::ApplySlot`). Everything else
described here is the contract that the outline slices R8 to R14 will
build.
It is not a [CC] milestone exit, a protocol approval or a release.
Every economic value below is a **PROVISIONAL REGTEST** default.

Companion files:

- [TODO.md](TODO.md) is the task ledger for Codex: every task ID, its status,
  its acceptance test, its evidence path and the provisional decisions D1 to D9.
- [README.md](README.md) describes C1 and points here.

Governing sources, in precedence order:

1. The owner's instruction of 2026-10-16: "make branch rebirth and implement
   there the outline and then we will instruct codex to complete everything".
2. The owner plan [b3-flowmesh-greenfield-plan-20261016.md](../../doc/design/b3-flowmesh-greenfield-plan-20261016.md)
   (cited as *plan*). Its Tier 1 decisions are used with the recommended
   defaults, **provisionally** (TODO.md, section "Provisional decisions").
3. The clean-core contract [b3-flowmesh-clean-core-20261016.md](../../doc/design/b3-flowmesh-clean-core-20261016.md)
   (cited as *[CC]*): milestones C1 to C9 and the seven ownership boundaries.
4. The requirements baseline (`greenfield/requirements.md`, about 180 IDs). It
   lives in a session scratch directory and is **not tracked** in this
   repository. TODO.md gives one-line definitions for the IDs it uses that plan
   §2.1 does not list.

Reference oracles (never production dependencies): `test/flowmesh_v2_model/`
(frozen accounting profile), `test/flowmesh_v2_agreement/PROTOCOL.md` (PBFT
profile), `test/flowmesh_v2_storage/PLAN.md` (disk recovery). V1 code
(`src/flowmesh/*`, `src/node/flowmesh_*`) is reference only. **Never wrap the V1
per-market ledger and call it a V2 account.**

---

## 0. Gates that are still open

These gates come from the outline review. Each row says how the slices treat
it; none of them is closed by the outline itself.

| Gate | What is open | Effect on the outline |
|---|---|---|
| **G-1** (O-1) | The tracked `CLAUDE.md` (`55c034ea`) locks Claude to `/Users/josh/development/ON/B3-FlowMesh` on branch `FlowMesh`. It also says that when an owner decision conflicts with tracked governing documentation, implementation stops until the documentation is reconciled in a reviewed commit. [CC] says "No new branch or PR", and plan §10.1 names `flowmeshV2-dev` as the implementation branch. The owner's 2026-10-16 instruction names branch `rebirth`. | The documentation commit followed the owner's direct instruction. The R1 implementation run received that instruction again, verbatim ("make branch rebirth and implement there the outline ..."), and treats it as the owner's explicit direction to implement the outline on `rebirth`. The slices therefore proceed, as isolated, default-OFF, unpushed code on `rebirth` only. **Still open:** the reconciliation of `CLAUDE.md`, [CC] and plan §10.1 has not landed. Nothing from `rebirth` may be merged into another branch, pushed or released until it lands or the owner rules otherwise. Claude does not edit `CLAUDE.md`. |
| **G-2** (O-14) | Decision D3 (the C-1 reconciliation: stable quote and fees in Q) has not landed. Plan §12.1 requires it before implementation. | The fee code in R4, R6 and R7 stays inside the REGTEST provisional profile. The owner acknowledges this or lands D3 first. |
| **G-3** (O-13) | A harness reminder asks for attribution trailers. `CLAUDE.md` forbids them. | No attribution trailers. Repository Git identity only. Never push. |
| **G-4** | Branch drift. | Before every edit and commit, `git -C /Users/josh/development/ON/B3-FlowMesh-hl branch --show-current` must print `rebirth`. Otherwise stop. |

---

## 1. What the outline is

The outline is a **walking skeleton** of the whole new architecture plus a
complete task ledger. Concretely:

- **Every module exists** with its real interface: a header whose comments state
  the contract (inputs, outputs, error codes, locks, determinism).
- **A minimal real implementation** exists wherever the end-to-end path needs
  it: one signed action goes through admission, shared application, a durable
  decision, a coherent read and engine-off client verification.
- **Everywhere else there is an explicit, test-visible stub.** A stub returns a
  typed `NotImplemented(TaskId)` or `Unsupported(TaskId)` error, or a certified
  typed refusal (for example `REJ_RESIDUAL_CROSS_UNSUPPORTED`). It never fakes
  success. Each stub carries `// TODO(rebirth:<task-id>)` within 3 lines, the
  ID exists in TODO.md, and a `stub_inventory` test case asserts it.
- A **REAL-REPLACE** skeleton is working code that is deliberately simple (for
  example a `std::map` order book). It is tagged the same way and must be
  replaced by the named task.

What the outline never does:

- It never links into `b3coind`, the wallet, Qt or `bitcoin_node`. Everything is
  behind default-OFF CMake options.
- It never changes consensus, wire format or fees of the existing node.
- It never contacts a real network, uses a real datadir or uses real keys.
- It claims no [CC] milestone exit (O-18). M0-AUD-01 audits it first.
- It makes no latency claim. Test runtime is not trading latency.

---

## 2. Module layout

Directory tree under `src/flowmesh2/`, as the slices create it:

```
src/flowmesh2/
  CMakeLists.txt            C1 block byte-identical, then if(BUILD_FLOWMESH2) ...
  README.md  ARCHITECTURE.md  TODO.md
  PROFILE_DEX1_REGTEST.json profile values; its profile_hash is checked against code
  mailbox.{h,cpp} read_slot.{h,cpp} foundation_tests.cpp read_slot_tests.cpp
                            C1 sources, frozen by SHA-256 (section 13.4)
  C1_RESULTS.md TEST_PROFILE.json            C1 records, unchanged
  core/        fm2_types     deterministic value types, codecs, spec functions
  ledger/      fm2_core      deterministic shared spot ledger and commitment v0
  engine/      fm2_core      deterministic order book, A2 sweep, slot phases
  state/       fm2_core      deterministic single-writer state owner
  storage/     fm2_core      journals, strict commit, durable tokens, BLS signer
  chain/       fm2_core      chain adapter seam (b3d adapters are stubs)
  agreement/   fm2_core      deterministic messages and agreement interfaces
  admission/   fm2_core      admission order A0..A10, bounded action pool
  read/        fm2_core      immutable slot views over C1 ReadSlot
  verify/      fm2_verify    b3verify, deterministic, never links fm2_core
  net/         fm2_core      service lanes over C1 BoundedMailbox, client API
  bridge/ futures/ governance/  fm2_core   interfaces only, every method a stub
  dev/         fm2_dev       REGTEST-only dev certifier, fake chain, dev budget
  sim/         fm2_simlib, fm2-sim   deterministic simulator and walking skeleton
  test/        test_flowmesh2  Boost.Test suites, compile_fail/, INVENTORY.txt
contrib/flowmesh2/          guards, inventory and TODO lint, evidence tool, canaries
.github/workflows/flowmesh2.yml   owner-protected; never executed by the outline
```

`src/CMakeLists.txt` is not edited. The new top-level option `BUILD_FLOWMESH2`
requires `BUILD_FLOWMESH2_FOUNDATION_TESTS=ON` (and therefore `BUILD_TESTS=ON`),
so the existing `add_subdirectory(flowmesh2)` hook is reused.

### 2.1 Modules, what is real and what is stubbed

"Slice" is the outline slice that creates the module. Task IDs are TODO.md IDs.

| Module | Slice | Real in the outline | Typed stubs (task) | REAL-REPLACE (task) |
|---|---|---|---|---|
| `core/result`, `core/codes` | R1 | `ErrorCode` 1..17, `Error`, `[[nodiscard]] Result<T>` with `Result() = delete` and `Ok()`, `TaskId`, `NotImplemented`, `Unsupported`; `OutcomeCode` and `AdmissionCode` registries | — | — |
| `core/ids`, `amount`, `hashes`, `codec`, `params`, `authority`, `market`, `merkle`, `slot` | R3 | `AssetId`, `AccountId::FromKey`, `MarketId`, `ActionId`, `OrderId::FromPlace`, `Outpoint`; checked `Atoms` (`AddAtoms`, `SubAtoms`, `MulChecked`, `MulDivFloor`/`Ceil`); tag registry (29 tags); `DecodeExact`, `ReadCappedVector`, `WriteCappedVector`; `NetworkId`, `ProfileParams`, `DevRegtestProfile`, `Validate`, `EncodeProfile`, `ProfileHash`, `DomainHash`; `Anchor`, `FactCursor`, `AuthorityVersion`; `MarketDescriptor`, `BaseAtoms`, `QuoteAtoms`; size-bound RFC 6962 Merkle (O-25); `SlotHeaderV2`, `GenesisHeader` | `Validate()`: price band (M3-ENG-03), `PER_SIDE` fees (M3-ENG-08), futures (M9-FUT-01), `FOLDED` signer (M4-STO-05) | per-market tick/lot (M3-ENG-11) |
| `core/action`, `body`, `outcome`, `facts`, `certificate`, `seat_set`, `account_leaf`, `nonce_rule`, `fee_math` | R4 | `ActionV2` exact capped codec, `UnsignedMsg`, `Id`, `Account`, BIP340 `VerifySignature`, `MaxDelayValid`, PLACE/CANCEL/TRANSFER/WITHDRAW payload codecs, `KindSupported`; `VerifiedAction::Verify`; `SlotBody` codec, `BodyHash`, `ActionLeaf`, `ActionsRoot`, `FactsRoot`; `OutcomeV2` codec and `LeafHash`; `DepositFact` codec and `FactId`; `SlotCertificate` codec, `Hash`, `SignerIndices`, `MakeBitmap`; `SeatSet::FromPoP`, `Quorum`, `Root`, `Keys`; `AccountLeafV0` codec and `LeafValue`; `ClassifyNonce`, `NonceInsert`, `InExpiryWindow`, `ExpiredAt`; `SellerFeeTotal`, `SellerFeeIncrement`, `SplitSlotFees` | codecs and `FactId` of five facts: seat change (M7-SEAT-01), listing (M7-GOV-01), checkpoint accepted (MB1-B3D-02), payout confirmed (M8-BRG-02), exit nullifier (M8-EXIT-01); `BatchVerifier::VerifyBatch` (M4-ADM-01) | nonce rule (M2-ADM-01) |
| `ledger/`, `core/state_root` | R5 | `NonceWindow` (over the core D7 rule), `AdmissionCodeFor`; `LedgerState` (deposit with outpoint nullifier, reserve, release, pay from reserved with fee withholding, credit to protocol accounts, transfer, nonces, open orders, `CheckConservation`), all atomic; protocol accounts `SeatPoolAccount`/`TreasuryAccount`; `AssetTotals`, `TotalsLeafV0`, `MarketLeafValue`, `NullifierLeafValue`, `StateRootsV0`; `CommitV0`, `ProveAccountV0` | `RequestWithdrawal` (M2-LED-05), `TransferToFutures`/`TransferFromFutures` (M2-LED-06), `CommitV1` (M2-LED-02), `LiabilitySumRoot` (M2-LED-03), `ExitValue` (M2-LED-04) | duplicate-fact rules (M2-LED-07), seat-pool account (M7-FEE-01) |
| `engine/book`, `matcher`, `residual` | R6 | `RequiredReservation`; `OrderBook` (exact reservations, never crossed, FIFO by `arrival_rank`, `FillBestFront` at the maker price) and `BookSet`; `AllocateProRata`; `SweepA2` with pairing, ledger settlement, seller fees, per-order and per-slot fill caps and `TakerStop`; `FindResidualCross` | `ResolveResidualCross` (M3-ENG-02), `PriceBandCheck` (M3-ENG-03) | book (M3-ENG-01), pairing and remainder rule (M3-ENG-14), self-trade (M3-ENG-13), slot fill budget (M3-ENG-04) |
| `engine/engine`, `core/state_root` (order leaf), `ledger/state_commitment` (trading state) | R7 | `OrderKey`, `CanonicalOrder`, `SlotPhase`, `PhaseOf`, `ArrivalRank`; `EngineState` (`Genesis`, `Markets`, `OrderLeaves`, `Commit`, slot-boundary `CheckInvariants`); `HeadView`; `SlotInputs`; `ValidateSlotInputs` → `ValidatedSlot` (private constructor); `Engine::Create`, `Engine::ApplySlot` phases 1-6, 8, 9 with outcomes, delta reconciliation and roots; `RestingOrderLeafV0`; `CommitTradingStateV0`, `OrderLeafOf` | `PhaseOf` branches `CANCEL_ALL` (M3-ENG-09), `WITHDRAW` (M2-LED-05), futures transfers (M2-LED-06); `RunFuturesPhase` (M9-FUT-04); residual cross as the certified refusal `REJ_RESIDUAL_CROSS_UNSUPPORTED` (M3-ENG-02) | order seed (M3-ENG-12), caps and the slot fill budget (M3-ENG-04), validity predicate (M4-AGR-11) |
| `storage/` | R8 | record framing, `Journal`, `MemoryJournal`, `FileJournal::CreateNew`, `JournalCommit`, `JournalDirectoryCommit`, `Pending<T>`, minting of the `Durable*` tokens (type landed in R2), `SignerLog`, `SeatSigner`, `CommitLog`; `agreement/outbound.h` | `OpenExisting`/`ReadAll` (M4-STO-01), `LogWriter` (M4-STO-02), `SnapshotStore` (M4-STO-03), folded signing (M4-STO-05), `Rollover` (M4-STO-06), view-state records (M4-STO-07) | — |
| `state/` | R9 | `StateOwner::Preview`, `Reexecute`, `Apply`, `Head`; `ReplayDecisions`; `CoreState::Genesis` | `Restore` (M4-STO-03), pipelined `Preview` over an uncommitted parent (M4-AGR-10) | full-state copy (M2-LED-10) |
| `chain/`, `agreement/`, `dev/` | R10 | `ChainAdapter` interface; capped message codecs; `Agreement` and `SafetyRules` interfaces; `DevSingleSeatCertifier` (k=1, REGTEST); `FakeChainAdapter` | `PbftSafetyRules` (M4-AGR-01), `PbftAgreement` (M4-AGR-02, also `DevSingleSeatCertifier::OnMessage`), `LeaderSchedule` (M4-AGR-03), `InclusionList` (M4-AGR-04), `RestartFence` (M4-AGR-06), `EvidenceStore` (M4-AGR-07), trusted-facts b3d adapter (MB1-ADP-06), finalized b3d adapter (M7-ADP-01) | — |
| `read/`, `verify/` | R11 | `SlotView`, `CertifiedEnvelope`, `ReadService`; `VerifyCertificate`, `VerifyOutcome`, `VerifyAccountLeaf`, `VerifyFill`, `VerifyBalanceTransition`, `VerifyExpired` | `VerifyLiabilityPath` (M5-VFY-03), `VerifyCheckpoint` (M8-CKP-01), `VerifyAnchorProof` (M7-ADP-02) | archive (M5-RD-01), v0 tree levels (M2-LED-02) |
| `admission/`, `dev/dev_budget` | R12 | `Admission` A0..A10, `ActionPool`, `BudgetPolicy`, `UnlimitedDevBudget` | `CreditBudget` (M4-ADM-02), batch verification (M4-ADM-01); `UNSUPPORTED_KIND` refusals (M2-LED-05, M3-ENG-09, M2-LED-06) | — |
| `net/`, `bridge/`, `futures/`, `governance/` | R13 | `LaneSet`, `ClientApi`, `InProcessApi`, `SeatMesh` interface | HTTPS + SSE (M5-API-01), `Subscribe` (M5-API-02), in-process mesh (M4-SIM-01), TCP mesh (M4-NET-01), short IDs (M4-NET-02), sentry fan-out (M4-NET-03), bridge (M8-BRG-01..03), checkpoint builder (M8-CKP-01), forced exit (M8-EXIT-01), futures (M9-FUT-01..04), governance (M7-GOV-01..03) | — |
| `sim/` | R14 | `FakeClock`, `Scheduler`, `FaultPlan` (3 faults), `TraceRecorder`, `SimNode`, `SimClient`, scenarios, `fm2-sim` | `SimCluster` n>1 (M4-SIM-01), crash/drop/partition/skew (M4-SIM-02), Twins (M4-SIM-03) | scheduler (M0-SIM-01) |
| `contrib/flowmesh2/` | R1, R2 | `check_inventory.py`, `check_todo.py`, `check_guards.py` (+ `test_check_guards.py`, `guard_fixtures/`), `check_negative_compile.py`, `evidence.py` (+ `test_evidence.py`), `canaries/` | `evidence.py perf-metadata` (M0-EVD-01) | — |
| `storage/durable.h`, `test/compile_fail/` | R2 | `Durable<K>` token type (no minter until R8); missing-`LOCK`, ignored-`Result` and forged-token controls with fixed forms and a positive control | — | — |

### 2.2 CMake targets (only when `BUILD_FLOWMESH2=ON`)

| Target | Contents | Links |
|---|---|---|
| `flowmesh2_foundations` | C1, unchanged | as C1 |
| `fm2_warnings` | INTERFACE: Clang/AppleClang `-Wthread-safety -Werror=thread-safety`; all compilers `-Werror=unused-result` | — |
| `fm2_types` | `core/` | `core_interface` and, since R4, `secp256k1` (private); `bitcoin_consensus` (`hash.cpp`, `uint256.cpp`, `pubkey.cpp` with `XOnlyPubKey::VerifySchnorr`), `bitcoin_crypto` (SHA-256 and `crypto/bls`) and `bitcoin_util` (public, since R3). Never `bitcoin_common` |
| `fm2_verify` | `verify/` | `fm2_types`, `bitcoin_crypto` (never `fm2_core`) |
| `fm2_core` | ledger, engine, state, storage, chain, agreement, admission, read, net, bridge, futures, governance; application code lives in namespaces `flowmesh2::ledger`, `::engine`, `::state`, `::storage` (guard rule 3 matches them) | `fm2_types` (public, since R5); `flowmesh2_foundations`, `crc32c` and `bitcoin_util` join with the slices that need them |
| `fm2_dev` | `dev/` (compiled with `FM2_DEV_COMPONENTS`) | `fm2_core` |
| `fm2_simlib` | `sim/` | `fm2_dev`, `fm2_core`, `fm2_verify`, `bitcoin_common` (for `CKey` only) |
| `test_flowmesh2` | `test/` | all of the above, `Boost::headers`, `univalue` (R3: the profile JSON check), `bitcoin_common` and `secp256k1` (since R4, for `CKey` in `test/util/keys.h` only; the nm guard confirms no V1 symbol is linked in); compiled with `FM2_SOURCE_ROOT` so tests can read tracked files; global fixture `ECC_Context` (`test/main.cpp`) |
| `fm2_sim_cli` | binary `fm2-sim` | `fm2_simlib` |
| `fm2_canary_*` | sanitizer canaries | only with `FLOWMESH2_SANITIZER_CANARIES=ON` |
| `fm2_all` | custom target: every fm2 library and executable that is built by default | — |

`file(GENERATE)` writes `fm2_libs.manifest` (from R2) into the build directory
(`fm2_libs-<CONFIG>.manifest` for multi-config generators). It lists every
target of `src/flowmesh2/` (CMake `BUILDSYSTEM_TARGETS`, so no target can be
left out) with its type, file, objects and configure-time link closure. The
`nm` guard reads it and fails if a listed library or executable was not built,
so no library can be skipped silently. Its section of
`src/flowmesh2/CMakeLists.txt` must stay last, and `src/flowmesh2/` has exactly
one CMake file. `bitcoin_common`, `bitcoin_node`, `bitcoin_wallet` and
`bitcoinqt` never appear in the link closure of `fm2_types`, `fm2_verify` or
`fm2_core`, and `fm2_core` never appears in that of `fm2_verify`.

### 2.3 Header index

Every public header, the slice that adds it and its contract in one line. A
header marked *landed* is normative; the others are the target contract.

| Header | Slice | State | Contract |
|---|---|---|---|
| `core/result.h` | R1 | landed | `ErrorCode` 1..17 (append-only), `Error{code, detail}` (detail static), `TaskId` (consteval, TODO.md ID format), `NotImplemented`/`Unsupported`, `[[nodiscard]] Result<T>` and `Result<void>` without default constructor, success only through `Ok()`/`Ok(v)` |
| `core/codes.h` | R1 | landed | `OutcomeCode` (certified) and `AdmissionCode` (not certified) registries with bands, `ToString` (total), `IsKnown`, `IsRejection` (unknown values are neither success nor rejection) |
| `core/ids.h` | R3 | landed | distinct id types over `uint256`, total order by bytes, raw 32-byte encoding; `AssetId::Native()` is zero; `AccountId::FromKey` = `Tagged(ACCOUNT, xonly)`; `OrderId::FromPlace` |
| `core/amount.h` | R3 | landed | `Atoms` = `CAmount` in [0, 2^62]; operand out of range `INVALID_INPUT`, result out of range `ARITHMETIC_OVERFLOW`; 128-bit `MulDiv` |
| `core/hashes.h` | R3 | landed | append-only `HashTag` registry (`B3/FM2/<name>`, enumerator `NET_DOMAIN` for `DOMAIN`), `Tagged`, `TaggedWriter`, `TaggedObject` |
| `core/codec.h` | R3 | landed | `DecodeExactWith`/`DecodeExact` (`OVERSIZE` above max bytes or cap, `DECODE` for malformed or trailing bytes), `ReadCappedVector` (cap before allocation), `WriteCappedVector`, `EncodeBytes` |
| `core/params.h` | R3 | landed | `NetworkId`, `ProfileParams` (21 fields), `DevRegtestProfile(ChainType)`, `Validate` (network, structure, then the four stubs), `EncodeProfile`, `ProfileHash`, `DomainHash` (O-26) |
| `core/authority.h` | R3 | landed | `Anchor`, `FactCursor` (total order; slots consume `[facts_from, facts_to)`), `AuthorityVersion` |
| `core/market.h` | R3 | landed | `MarketDescriptor` (REAL-REPLACE M3-ENG-11), `MarketId`, `ValidateMarket`, checked `BaseAtoms`/`QuoteAtoms` |
| `core/merkle.h` | R3 | landed | `MerkleRoot` = `Tagged(MERKLE_ROOT, n ‖ MTH)`, `MerkleProof` (path cap 64), `MerkleProve`, `MerkleVerify` (RFC 9162) |
| `core/slot.h` | R3 | landed | `SlotHeaderV2` (18 fields, 366 bytes), `Hash` (`SLOT`), `CommitDigest` (`COMMIT`), `Authority`, `Encode`/`Decode`, `GenesisHeader` |
| `core/action.h` | R4 | landed | `ActionV2` (84-byte fixed prefix, kind at byte 83), `Decode(bytes, profile)` (OVERSIZE above `MaxEncodedSize` or the payload cap; DECODE with `ACTION_UNKNOWN_VERSION`/`ACTION_UNKNOWN_KIND` details), total `Encode`/`UnsignedMsg`, `Id` = `Tagged(ACTION, UnsignedMsg)`, `VerifySignature` over `Id`, `MaxDelayValid`; `ActionKind`, `Side`, `TimeInForce` registries; payload codecs; `KindSupported`; `VerifiedAction` (private constructor, `Verify` only); `BatchVerifier` stub (M4-ADM-01) |
| `core/body.h` | R4 | landed | `SlotBody{facts, originals}` as opaque bytes, capped exact codec, `BodyHash`, `ActionLeaf`, `FactLeaf`, `ActionsRoot`, `FactsRoot`; `MAX_SLOT_FACTS`, `MAX_FACT_BYTES` (O-27) |
| `core/outcome.h` | R4 | landed | `OutcomeSource`, `Fill`, `BalanceDelta`, `OutcomeV2` exact capped codec, `LeafHash` = `Tagged(OUTCOME, bytes)`; field meaning provisional until R6/R7 (O-30) |
| `core/facts.h` | R4 | landed | `FactKind` registry, `Fact` variant, `DepositFact` (117 bytes) codec, `FactId` = `Tagged(FACT, bytes)`, `DecodeFact`; five stub kinds |
| `core/certificate.h` | R4 | landed | `SlotCertificate{header_hash, bitmap, aggregate}` exact codec (bitmap at most 128 bytes), `Hash` = `Tagged(CERT, bytes)`, LSB-first `SignerIndices(k)` and `MakeBitmap` |
| `core/seat_set.h` | R4 | landed | `SeatSet::FromPoP` (only `bls::VerifiedPublicKey::FromPoP`; refuses empty, above `MAX_SEATS`, repeated key, bad PoP), `QuorumFor` = ⌊2k/3⌋+1, `Root`, `Keys` |
| `core/account_leaf.h` | R4 | landed | `AccountLeafV0` exact capped codec with the canonical form enforced, `LeafValue` = `Tagged(STATE_ACCOUNTS, bytes)`, lookups |
| `core/nonce_rule.h` | R4 | landed | D7 spec: `ClassifyNonce`, `NonceAcceptable`, `NonceInsert`, `InExpiryWindow`, `ExpiredAt` (REAL-REPLACE M2-ADM-01) |
| `core/fee_math.h` | R4 | landed | D5 spec: `SellerFeeTotal`, `SellerFeeIncrement` (ceiling on the cumulative per-order fee), `SplitSlotFees` (floor to seats, residual to treasury); PROVISIONAL, gate G-2 |
| `core/state_root.h` | R5, R7 | landed | commitment v0 value types shared with b3verify (O-33); R7 adds `RestingOrderLeafV0` (153 bytes, canonical form enforced, `LeafValue` = `Tagged(STATE_ORDERS, bytes)`) and leaves `COMMITMENT_VERSION_V0` to `core/slot.h` (O-43): `SeatPoolAccount`/`TreasuryAccount` = `Tagged(PROTOCOL_ACCOUNT, name ‖ profile_hash)`; `AssetTotals{deposits, effects, pending, fees_held}`; 64-byte `TotalsLeafV0` (exact decode, all-zero refused); `MarketLeafValue`, `NullifierLeafValue`; `StateRootsV0` (160 bytes) and `Root()` = `Tagged(STATE, u8 0 ‖ five roots)`; `COMMITMENT_VERSION_V0` |
| `test/util/keys.h` | R4 | landed | TEST keys from labels: `CKey` and `bls::SecretKey::FromIKM` from `SHA256(label)`, `SignDigest` |
| `test/util/ledger_fixtures.h` | R5 | landed | TEST fixtures for ledger and commitment tests: `TestAsset`, `TestAccount`, `TestOutpoint`, `Deposit`, markets M1/M2, `EmptyLedger`, `RequireOk`/`RequireError`/`RequireStub` (read `error()` only on failure) |
| `ledger/nonce_window.h` | R5 | landed | `NonceWindow` (`Create`, `FromUsed`, `Classify`, `Insert`; no rule of its own, the core D7 functions), `AdmissionCodeFor` (USED → `NONCE_USED`, TOO_LOW → `NONCE_TOO_LOW`) |
| `ledger/ledger.h` | R5 | landed | `LedgerState` value type, no lock, atomic operations (an Error or rejection changes nothing): `CreditDeposit`, `Reserve` (OK/INSUFFICIENT), `Release`, `PayFromReserved(from, to, asset, amount, withheld_fee)`, `Credit` (protocol accounts, from `fees_held`), `Transfer` (O-7 outcomes), `ConsumeNonce`, `AddOpenOrder` (OK/AT_CAP), `RemoveOpenOrder`, `Leaf`, `CheckConservation` (also a free function over explicit maps); stubs `RequestWithdrawal` (M2-LED-05), `TransferToFutures`/`TransferFromFutures` (M2-LED-06) |
| `ledger/state_commitment.h` | R5, R7 | landed | `CommitV0(ledger, markets, orders)` → `StateRootsV0`, `OrderLeafV0`, `ProveAccountV0` → `AccountWitnessV0{leaf, proof}`; R7: `OrderLeafOf`, `CommitTradingStateV0(ledger, markets, RestingOrderLeafV0s)` (refuses a non-canonical leaf, an unlisted market and an owner without an account); stubs `CommitV1` (M2-LED-02), `LiabilitySumRoot` (M2-LED-03), `ExitValue` (M2-LED-04) |
| `engine/book.h` | R6 | landed | `RestingOrder`, `Reservation`/`RequiredReservation`, `Opposite`, `Reaches`; `OrderBook` (REAL-REPLACE M3-ENG-01): `Create`, `BestBid`/`BestAsk`/`Best`, `WouldCross`, `Find`, `BestFront`, `LevelLots`, `Orders`, `Rest`, `Remove`, `FillBestFront`, `CheckInvariants`, five invariants kept by every mutation (O-35); `BookSet` (`Create`, `Find`, `FindOrder`, `CheckInvariants`) |
| `engine/matcher.h` | R6 | landed | `TakerStop` registry, `Taker`, `SlotFillBudget`, `ExecutedFill`, `SweepResult`; `AllocateProRata` (largest remainder, ties by rank; M3-ENG-14); `SweepA2(book, ledger, takers, profile, budget)`: preconditions first with no change, then BUY takers against asks and SELL takers against bids, settlement through the ledger (O-36, O-37); stub `PriceBandCheck` (M3-ENG-03) over `ReferencePrice` |
| `engine/residual.h` | R6 | landed | `FindResidualCross` (real detection); `ResidualCall` (target shape) and stub `ResolveResidualCross` (M3-ENG-02; O-38) |
| `engine/engine.h` | R7 | landed | `OrderingEntry`, `OrderKey` = `Tagged(ORDER, parent ‖ u64 height ‖ ActionId)`, `CanonicalOrder` (seeded sort, then each account's positions in ascending nonce; REAL-REPLACE M3-ENG-12); `SlotPhase` registry 1..9, `PhaseOf` (stub branches M3-ENG-09, M2-LED-05, M2-LED-06); `ArrivalRank` = (2·height + phase bit)·max_actions_per_slot + rank (O-39); `EngineState`; `HeadView` (non-owning, refuses temporaries); `SlotInputs`; the `SLOT_*` refusal details; `ValidateSlotInputs` (REAL-REPLACE M4-AGR-11, O-41) → `ValidatedSlot`; `SlotAction`, `ActionPayload`; `SlotResult`; `RunFuturesPhase` (stub M9-FUT-04); `Engine` (section 11) |
| `test/util/slot_fixtures.h` | R7 | landed | TEST fixtures: synthetic TEST network and domain, label keys `fm2/test/key/<name>`, `SlotFixture` (genesis head, signed `Place`/`Cancel`/`Transfer`, deposit bytes at the next cursor, `Inputs`, `Validate`, `TryApply`, `Apply` and `Advance`, which assembles the next header as R9 will), `OutcomeOf`, `RequireRefusal` |
| `test/util/engine_fixtures.h` | R6 | landed | TEST fixtures: `EngineOrder`, `EngineFixture` (fund, rest a maker the way phase 6 does, reserve a taker the way phase 5 does, sweep and check conservation and book invariants) |
| `test/reference_matcher.h` | R6 | landed | TEST oracle: naive O(n²) A2 matcher over plain vectors (scans for the best price, rank order, FIFO maker and count-ahead largest remainder); shares only the id types and `Side` with the engine |
| `storage/durable.h` | R2 | landed | `DurableKind`, move-only `Durable<K>` tokens with a private constructor whose only friend is `Journal` (R8 mints them); section 7.2 |
| `storage/*.h` (others), `agreement/outbound.h` | R8 | target | section 7 |
| `state/state_owner.h`, `candidate.h`, `replay.h` | R9 | target | sections 4, 7.4 |
| `chain/*.h`, `agreement/*.h`, `dev/*.h` | R10 | target | sections 3, 8 |
| `read/*.h`, `verify/b3verify.h` | R11 | target | section 10.2 |
| `admission/*.h`, `dev/dev_budget.h` | R12 | target | section 10.1 |
| `net/*.h`, `bridge/*.h`, `futures/*.h`, `governance/*.h` | R13 | target | section 2.1 |
| `sim/*.h` | R14 | target | section 14 |

---

## 3. Data flow of one slot

```
 client: exact ActionV2 bytes, stored before sending, never re-signed
   |
   v
 net/LaneSet ............ 4 C1 BoundedMailbox lanes; BUSY means "not queued"
   |
   v
 admission/Admission .... A0..A10 against ONE HeadView snapshot
   |                      (account, nonce, expiry, budget BEFORE BIP340)
   v
 admission/ActionPool ... bounded; TakeForSlot marks IN_PROPOSAL, does not remove
   |
   |   chain/ChainAdapter.Poll -> FactBatch | BUSY | NEED_DATA | AUTHORITY_CHANGED
   v
 SealSlot (sim/SimNode in the outline)
   |
   v
 state/StateOwner.Preview(ParentRef, SlotInputs)       pure, no lock while executing
   |  ValidateSlotInputs -> CanonicalOrder -> Engine::ApplySlot -> roots -> header
   v
 Candidate{header, outcomes, body, next state}
   |
   v
 agreement/Agreement.Propose      (outline: dev/DevSingleSeatCertifier, k = 1)
   |  [auth check 1] signer: ACCEPTED_PROPOSAL + INTENT --> B-INTENT
   |  [auth check 2] SeatSigner::Sign(DurableIntent)
   |                 signer: SIGNED_VOTE --> B-VOTE
   |                 certificate (bitmap 0x01)
   |                 commit: DECISION{header, cert, body} --> B-DECISION
   v
 Decision{DurableDecision, cert, body}
   |
   v
 StateOwner.Apply(Candidate, DurableDecision, current AuthorityVersion)
   |  [auth check 3]; single writer; pointer swap
   v
 commit: APPLIED{slot, state_root} --> B-APPLIED --> DurableApplied
   |
   v
 read/ReadService.Publish(DurableApplied, SlotView)   C1 ReadSlot, memory visibility
   |
   v
 ActionPool.OnCommitted   included removed; terminal NONCE_USED / EXPIRED
   |
   v
 client: GetAction / GetState  -> verify/b3verify: certificate, outcome,
         leaf, fill, fee, balance transition, EXPIRED proof
```

[CC] ownership boundaries map onto modules as follows:

| [CC] boundary | Module | Outline guarantee |
|---|---|---|
| 1 Admission | `net/`, `admission/` | Queue acceptance, admission, inclusion and verified execution are separate answers. Admission codes mean "not admissible now", never an execution outcome. |
| 2 Application | `ledger/`, `engine/`, `state/` | One deterministic state owner for all AssetIds and markets. `Preview` is pure and calls no wallet, Qt, file, network or chain code. |
| 3 Agreement | `agreement/`, `dev/` | The certificate covers the exact parent, the exact original action bytes in proposal order (via `actions_root`) and the resulting commitments. The k=1 dev certifier is labelled REGTEST-only and is not a quorum. |
| 4 Storage and signing | `storage/` | Intent durable before the first signature; signed object durable before it leaves; decision durable before apply; application durable before publication. Enforced by move-only tokens. |
| 5 Read publication | `read/` | Publication only after B-APPLIED. One reply carries one scope and one head. Proof, authority and freshness are separate fields. |
| 6 Chain adapter | `chain/` | Explicit `BUSY`, `NEED_DATA` and `AUTHORITY_CHANGED`. No `cs_main`. The sign-versus-reorg linearization is **not** proved (M10-SPEC-01). |
| 7 Client | `sim/SimClient`, `verify/` | Exact original bytes; never re-signs; verifies the applied result, fill, fees and deltas. |

---

## 4. Ownership and lock rules

1. **Bitcoin locks only.** `Mutex` and `LOCK` from `sync.h`, members
   `GUARDED_BY`, and every public locking method annotated
   `EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)` (the C1 pattern). Banned in all non-test
   fm2 code: `std::mutex`, `std::recursive_mutex`, `RecursiveMutex`,
   `GlobalMutex`, `std::shared_mutex`, `std::lock_guard`, `std::scoped_lock`,
   `std::unique_lock<std::mutex>`, `std::condition_variable_any`, `std::jthread`,
   `std::atomic<std::shared_ptr`.
2. **No lock across a wait.** No fm2 lock is held across disk, network, chain or
   signature work.
3. **No nesting.** No fm2 code holds two fm2 mutexes at once. Any future nesting
   must document its order here and add a `DEBUG_LOCKORDER` test (M0-CI-03).
4. **One writer.** `StateOwner` is the only mutator of trading state. State is an
   immutable `shared_ptr<const CoreState>`. `Preview` copies the head pointer
   under the lock and executes without it. `Apply` swaps the pointer under the
   lock. A `HeadView` reader can never see partial state.

| Component | Mutex | Protects | Held for |
|---|---|---|---|
| `StateOwner` | `m_mutex` | head pointer | pointer copy or swap only |
| `ActionPool` | `m_mutex` | pool entries, statuses, counters | map operations; never signature checks |
| `ReadService` | archive mutex | slot-view index | lookup or insert; never I/O (REAL-REPLACE M5-RD-01 removes it from reads) |
| each `Journal` | `m_mutex` | buffer, `seq`, `durable_seq`, fatal flag | append; `Barrier` copies the target `seq`, releases the lock, runs the commit, then re-locks to record the result |
| `InProcessApi` | `m_mutex` | reply map | map operations |
| `FakeChainAdapter` (dev) | `m_mutex` | scripted batches, authority | scripted reads |
| C1 `BoundedMailbox`, `ReadSlot` | internal | as C1 | as C1 |

In the outline, barriers on one journal are issued by one thread (the sealing
thread). The group-commit `LogWriter` (M4-STO-02) takes over barriers later.

---

## 5. Determinism rules

Deterministic directories: `core/`, `ledger/`, `engine/`, `state/`,
`agreement/`, `verify/`. In them the guard bans: `float`, `double`,
`std::unordered_*`, `std::hash`, `<chrono>`, `<random>`, `<thread>`,
`<fstream>`, `<cstdio>`, `fopen`, `fs::`, `std::condition_variable`, `GetTime`,
`rand(`, `random_device`, `FastRandomContext`, `GetRand` and logging.

- Every comparator is a total order: key, then `ActionId`.
- Arithmetic uses `util/overflow.h` and `util::Unsigned128` (`util/int128.h`).
  Atoms are `int64` in [0, 2^62]. Overflow is an error, never a wrap.
- Codecs use Bitcoin `serialize`/`streams`. Every `CompactSize` is checked
  against its cap **before** allocation. Trailing bytes are refused.
- Time enters only as a parameter (`OnTimeout(now_ms)`). The simulator's
  `FakeClock` and seeded `FastRandomContext(uint256)` live in `sim/` only.

---

## 6. Error model and stubs

- `Result<T>` is a `[[nodiscard]]` wrapper over `util::Expected<T, Error>` with
  `Result() = delete`. A bare `return {};` cannot produce success, and the guard
  also bans it. `-Werror=unused-result` turns an ignored result into a build
  error. Compile-fail CTests prove both.
- `ErrorCode` (append-only): `NOT_IMPLEMENTED=1`, `UNSUPPORTED=2`,
  `UNSUPPORTED_NETWORK=3`, `DECODE=4`, `OVERSIZE=5`, `ARITHMETIC_OVERFLOW=6` (O-21),
  `INVARIANT=7`, `ORDER_VIOLATION=8`, `EQUIVOCATION_REFUSED=9`,
  `STORAGE_FATAL=10`, `FENCED=11`, `AUTHORITY_CHANGED=12`, `VERIFY_FAILED=13`,
  `STALE_PARENT=14`, `BUSY=15`, `NEED_DATA=16`, `INVALID_INPUT=17`.
- `OutcomeCode` (certified, append-only): `DEPOSIT_CREDITED`, `RESTING`,
  `FILLED`, `PARTIAL_RESTING`, `IOC_DONE`, `CANCELLED`, `TRANSFERRED`,
  `FEES_DISTRIBUTED`, and the rejections `REJ_INSUFFICIENT_AVAILABLE`,
  `REJ_UNKNOWN_MARKET`, `REJ_UNKNOWN_ORDER`, `REJ_NOT_OWN_ORDER`,
  `REJ_ORDER_CLOSED`, `REJ_WOULD_CROSS`, `REJ_BOOK_FULL`, `REJ_FILL_CAP`,
  `REJ_PRICE_BAND`, `REJ_OPEN_ORDER_CAP`, `REJ_BAD_PARAMS`,
  `REJ_UNKNOWN_ASSET`, `REJ_NO_RECIPIENT`, `REJ_RESIDUAL_CROSS_UNSUPPORTED`.
- `AdmissionCode` (not certified): `ADMITTED`, `DUPLICATE`, `MALFORMED`,
  `OVERSIZE`, `WRONG_DOMAIN`, `UNKNOWN_KIND`, `UNSUPPORTED_KIND`, `NO_ACCOUNT`,
  `EXPIRED`, `REF_SLOT_AHEAD`, `BAD_EXPIRY`, `NONCE_TOO_LOW`, `NONCE_USED`,
  `NONCE_PENDING`, `BUDGET`, `BAD_SIG`, `BUSY`, `POOL_FULL`, `HALTED`.
- A stub is `return NotImplemented("M2-LED-05"); // TODO(rebirth:M2-LED-05)` or
  the `Unsupported` form. `TaskId` has a `consteval` constructor, so the ID is a
  literal, and a literal that does not match the TODO.md ID format does not
  compile (`IsWellFormedTaskId`, the same pattern `check_todo.py` uses). The
  guard checks the tag, TODO.md and the `stub_inventory` case.

As landed in R1 (`core/result.h`, `core/codes.h`):

- `Ok(v)` returns an `OkValue<T>` tag and `Ok()` an `OkVoid` tag; only these
  convert to a successful `Result`. There is no implicit conversion from `T`,
  so `return 5;` does not compile. An `Error` converts implicitly, so
  `return Error{ErrorCode::DECODE, "..."};` and `return NotImplemented(...);`
  work. `Error` has no default constructor, and its `detail` must point to
  static storage (a literal or a `TaskId`).
- `value()` on an error throws `util::BadExpectedAccess`; `error()` on a value
  fails an `Assert`. Callers test `has_value()` first.
- Negative compile probes run by hand for R1: an ignored `Result<int>`,
  `Result<void>` or `Error`, `return {};` for `Result<int>` and `Result<void>`,
  `return 5;`, a runtime `const char*` task ID and a malformed task ID literal
  each fail to compile with the fm2 flags; a positive control compiles. The R2
  CTest `flowmesh2_negative_compile` (Clang) makes the missing-`LOCK`, ignored
  `Result` and forged-token controls permanent (section 13.3).
- Code integers. `OutcomeCode`: 1-63 executed (`DEPOSIT_CREDITED=1` ..
  `FEES_DISTRIBUTED=8`), 64-95 rule rejections (`REJ_INSUFFICIENT_AVAILABLE=64`
  .. `REJ_NO_RECIPIENT=76`), 96-127 certified typed refusals of features not
  implemented yet (`REJ_RESIDUAL_CROSS_UNSUPPORTED=96`). `AdmissionCode`: 1-15
  queued or already known (`ADMITTED=1`, `DUPLICATE=2`; neither is a
  rejection), 16-63 not admitted (`MALFORMED=16` .. `HALTED=32`). Zero is never
  a code. A new code takes the next free integer of its band; `codes_tests`
  and `result_tests` pin every integer and name.
- Unknown values: `IsKnown` false, `IsRejection` false, `ToString` "UNKNOWN".
  Decoders refuse them with `DECODE`; nobody infers success from
  `!IsRejection`.
- `test/stub_inventory_tests.cpp` holds the inventory. Each stub gets one case
  written `FM2_STUB_CASE(M2_LED_05, "M2-LED-05") { ... return Observe(...); }`;
  the case passes only for `NOT_IMPLEMENTED`/`UNSUPPORTED` with exactly that ID
  as detail, and `registry_selfcheck` re-checks every registered stub. Test
  files may call `NotImplemented`/`Unsupported` as fixtures; the stub-site rule
  of the R2 guard applies to non-test sources.

---

## 7. Durability barriers

### 7.1 Records and journals

`RecordType` (append-only): `INTENT=1`, `SIGNED_VOTE=2`, `DECISION=3`,
`APPLIED=4`, `SNAPSHOT_MARK=5`, `ACCEPTED_PROPOSAL=6`, `VIEW_STATE=7`,
`NEW_VIEW_ACCEPTED=8`, `PREPARED_QC=9`.

| Journal role | Accepts | Rule |
|---|---|---|
| `SIGNER` | 1, 2, 6, 7, 8 | Never deleted (DUR-2). Loss, a gap or a CRC break means key retirement (M4-STO-01, M4-AGR-06). |
| `COMMIT` | 3, 4, 5, 9 | `DECISION` carries the slot **body**, so a decided slot can be re-executed after a crash (O-16). |

Framing: `[u32 len][u32 crc32c(type ‖ payload ‖ chain)][u8 type][payload][32 B chain]`,
where `chain = SHA256(prev ‖ type ‖ payload)` and the first `prev` is
`Tagged(B3/FM2/JOURNAL, role ‖ instance_id)`. `ScanRecords` detects CRC, chain,
length and cross-instance splice errors.

Strict commit, because Bitcoin's `FileCommit`, `DirectoryCommit` and
`AllocateFileRange` ignore errors (`src/util/fs_helpers.cpp`):

- `JournalCommit`: `fdatasync` on Linux; `F_FULLFSYNC` on macOS with **no**
  `fsync` fallback; `FlushFileBuffers` on Windows. Any error, including
  `EINVAL`, sets a sticky fatal flag. It is never retried.
- `JournalDirectoryCommit`: `fsync` of the directory descriptor. Any error is
  fatal.
- `FileJournal::CreateNew` refuses a non-empty directory, writes explicit zero
  pages for the whole segment, then runs both commits.
- After a fatal error: `Append` returns `FENCED`, `Barrier` returns
  `STORAGE_FATAL`, `Confirm` is refused, and the node fences admission and
  sealing. Reads stay live on the last published head.

### 7.2 Tokens make the order a compile-time property

`Append*` returns `Pending<Kind>{seq, key}`. `Barrier()` returns the durable
sequence. `Confirm(pending)` returns a move-only `Durable*` token **only if**
`DurableSeq() >= seq` and the journal is not fatal. Tokens have private
constructors and are bound to (instance, seq, slot, view, digest or header hash).

| Token | Only way to... |
|---|---|
| `DurableIntent` | call `SeatSigner::Sign` (the only BLS signing path) |
| `DurableSignedVote` | build an `OutboundVote` (the only way to send a vote) |
| `DurableDecision` | call `StateOwner::Apply` and `CommitLog::AppendApplied` |
| `DurableApplied` | call `ReadService::Publish` |

As landed in R2 (`storage/durable.h`, header-only): `DurableKind`
(`INTENT=1`, `SIGNED_VOTE=2`, `DECISION=3`, `APPLIED=4`) and the class
template `Durable<K>` with the four aliases above. No default constructor, no
copy, no move assignment; the only constructor is private and `class Journal`
is its only friend. A token carries the journal instance and the sequence
number; R8 adds `Journal::Confirm` (the only minter), the slot/view/digest
binding and the moved-from semantics. The compile-fail control
`test/compile_fail/forge_token.cpp` proves that a construction outside
`Journal` does not compile.

Equivocation: `VoteKey{domain, epoch, slot, view, phase}` maps to exactly one
digest forever. The same digest again is idempotent; another digest returns
`EQUIVOCATION_REFUSED`. A new `view` is a new key, so legitimate re-votes after a
view change are allowed.

### 7.3 Barrier map (k = 1 dev certifier, 4 barriers per slot)

| Barrier | Journal | Covers | Must complete before | Token |
|---|---|---|---|---|
| **B-INTENT** | signer | `ACCEPTED_PROPOSAL`, `INTENT` | any BLS signature for that `VoteKey` | `DurableIntent` |
| **B-VOTE** | signer | `SIGNED_VOTE` | the vote leaves the process or enters a certificate | `DurableSignedVote` |
| **B-DECISION** | commit | `DECISION{header, cert, body}` | `Apply` and any report of the slot | `DurableDecision` |
| **B-APPLIED** | commit | `APPLIED{slot, state_root}` | read publication and pool terminal statuses | `DurableApplied` |

Full PBFT (M4-AGR-02) adds proposal and prepare-phase barriers (the plan's
"strict 9"). Group commit (M4-STO-02) may cover several records with one
barrier but never lets a token exist before its barrier. Folded signing
(M4-STO-05, decision D15) stays a stub; strict mode is the only mode.

### 7.4 Recovery in the outline

- **Real:** `ReplayDecisions(genesis, records)` re-executes `DECISION` bodies
  exactly once per height from a `MemoryJournal` written by the real
  `CommitLog`, and reproduces every header and the final root. A duplicated
  `DECISION` is applied once.
- **Stub:** reopening a file journal (M4-STO-01), snapshots and `Restore`
  (M4-STO-03), restart high-water statements from 2f+1 seats (M4-AGR-06).
  The outline therefore makes **no crash-recovery claim for real files**.

---

## 8. Authority versioning

- `AuthorityVersion{epoch, seat_set_root, profile_hash}`. The slot header binds
  `authority_epoch`, `seat_set_root` and `profile_hash`.
- The epoch changes only when a seat-set fact is finalized, or when a
  disconnect removes a block at or below the current anchor (plan PLAN-11). In
  the outline only `FakeChainAdapter::BumpAuthority` changes it. The
  synchronous b3d hook is MB1-ADP-07.
- `ChainAdapter`: `Network()`, `CurrentAuthority()`, `CheckAuthority(v)`
  (returns `AUTHORITY_CHANGED`), `Poll(after, max)` (returns a `FactBatch`,
  `BUSY`, `NEED_DATA` or `AUTHORITY_CHANGED{rewind_to}`),
  `VerifyAnchor(candidate, last_certified)`, `SeatSetAt(epoch)`.

Three checks per slot (CONS-10 requires both signing and commit to check):

| Check | Where | If authority changed |
|---|---|---|
| 1 | before `ACCEPTED_PROPOSAL` | `AUTHORITY_CHANGED`; nothing written |
| 2 | after B-INTENT, immediately before `Sign` | `AUTHORITY_CHANGED`; the `INTENT` is durable but no signature exists; another digest for the same key stays refused |
| 3 | `Apply`: `header.authority_epoch` against the current epoch | `AUTHORITY_CHANGED`; head unchanged; the node fences admission and sealing |

This narrows the sign-versus-reorg window. It is **not** a proof. [CC]
boundary 6 requires the linearization to be proved before native signing
integration (M10-SPEC-01). The outline has no native signing integration.

Other versioned formats: `ActionV2.version = 1`, `SlotHeaderV2.version`,
`commitment_version = 0`, and the append-only registries (`ErrorCode`,
`OutcomeCode`, `AdmissionCode`, `RecordType`, hash tags). Parameter changes at
versioned heights are M7-GOV-03.

---

## 9. Canonical objects and hashing

Every tag is prefixed `B3/FM2/` and lives in one `constexpr` registry:
`DOMAIN`, `ACCOUNT`, `ACTION`, `ACTION_LEAF`, `BODY`, `FACT`, `FACTS`,
`MARKET`, `ORDER`, `SLOT`, `COMMIT`, `PREPARE`, `CERT`, `OUTCOME`, `SEATSET`,
`PROFILE`, `STATE`, `STATE_*`, `MERKLE_LEAF`, `MERKLE_NODE`, `MERKLE_EMPTY`,
`PROTOCOL_ACCOUNT`, `JOURNAL`, `TEST_ASSET`. No tag may collide with an existing
`B3/` tag string in the repository.

As landed in R3 (`core/hashes.h`): 29 tags with pinned integers 1..29 in the
order above, `STATE_*` being `STATE_ACCOUNTS`, `STATE_ORDERS`,
`STATE_MARKETS`, `STATE_NULLIFIERS` and `STATE_TOTALS`, plus `MERKLE_ROOT`
after `MERKLE_EMPTY` (O-25). The enumerator of `B3/FM2/DOMAIN` is
`NET_DOMAIN` (O-21). `hashes_tests` pins each `Tagged(tag, "")` against
Python goldens and scans `src/`, `contrib/`, `test/` and `contracts/` for
existing `"B3/..."` literals: none equals an fm2 tag and none uses the
`B3/FM2/` prefix.

- `DomainHash = Tagged(DOMAIN, ModernChainDomain ‖ fm_version ‖ magic)`.
  `ProfileHash = Tagged(PROFILE, canonical profile encoding)`.
- `AccountId = Tagged(ACCOUNT, xonly_pubkey)`. An account exists once a deposit
  fact creates it.
- `ActionV2`: version, domain, signer, nonce, ref_slot, max_delay (≤ 1,024),
  kind, payload (≤ 4,096 B), BIP340 signature.
  `ActionId = Tagged(ACTION, all fields except the signature)`. The BIP340
  message is the `ActionId` (O-8). `OrderId` = the PLACE's `ActionId`.
  Supported kinds in the outline: `PLACE`, `CANCEL`, `TRANSFER`.
- `SlotBody{facts, originals}` holds the **exact original action bytes** in
  proposal order. `ActionLeaf = Tagged(ACTION_LEAF, exact bytes)`, so two valid
  signatures on one `ActionId` are distinct leaves.
- `SlotHeaderV2`: `version, domain, profile_hash, authority_epoch, height,
  parent_hash, parent_cert_hash, anchor, facts_from, facts_to, seat_set_root,
  facts_root, actions_root, outcomes_root, state_root, action_count,
  outcome_count, commitment_version = 0`. `Hash()` uses tag `SLOT` (identity and
  parent link). `CommitDigest()` uses tag `COMMIT` (the BLS message). The fields
  `profile_hash`, `authority_epoch` and `facts_root` are additions to the plan's
  header text (O-20).
- `SlotCertificate{header_hash, bitmap, aggregate96}`. The bitmap is
  LSB-first: seat 0 is byte `0x01`. Quorum is ⌊2k/3⌋+1.
- Merkle: RFC 6962 shape with tagged leaf, node and empty hashes, because
  `consensus/merkle.cpp` has the duplicate-leaf flaw (CVE-2012-2459). The
  committed root is `Tagged(MERKLE_ROOT, u64 n ‖ MTH)`, so a proof cannot
  claim another tree size for the same root (O-25).
- `SlotHeaderV2` as landed in R3: fixed 366-byte encoding in the field order
  above; `anchor` is `u32 height ‖ hash`, each `FactCursor` is
  `u32 height ‖ u32 index`, counts are `u32`, `version = 1`. Decode refuses
  another `version` or `commitment_version` with `DECODE`.
- As landed in R4 (all PROVISIONAL REGTEST):
  - `ActionV2` encodes as `u8 version ‖ domain ‖ signer ‖ u64 nonce ‖ u64
    ref_slot ‖ u16 max_delay ‖ u8 kind ‖ CompactSize payload ‖ 64 B sig`.
    `ActionKind` follows plan §3.6: `PLACE=1, CANCEL=2, CANCEL_ALL=3,
    WITHDRAW=4, TRANSFER=5, SPOT_TO_FUTURES=6, FUTURES_TO_SPOT=7` (O-28).
    `Decode` refuses an unknown version or kind with static details that
    admission maps to `WRONG_DOMAIN`/`UNKNOWN_KIND` (O-29); `max_delay`
    outside 1..`max_delay_slots` is flagged by `MaxDelayValid`, not refused,
    so admission can answer `BAD_EXPIRY`.
  - `SlotBody` encodes `CompactSize #facts, (len ‖ bytes)…, CompactSize
    #originals, (len ‖ bytes)…`. Facts are canonical `EncodeFact` bytes, so
    `FactLeaf(bytes)` = `FactId(fact)` and `facts_root` is the Merkle root of
    the FactIds (O-30).
  - `OutcomeV2` encodes `u8 source ‖ subject ‖ account ‖ u64 nonce ‖ u16
    code ‖ u64 remaining_lots ‖ fills ‖ deltas`; a `Fill` is 137 bytes
    (including `seller_quote_before`, so `VerifyFill` can recompute the fee
    increment), a `BalanceDelta` 80 bytes. A `SLOT` outcome's subject is the
    quote `AssetId` of its `FEES_DISTRIBUTED` outcome.
  - `DepositFact` is `u8 1 ‖ cursor ‖ outpoint ‖ account ‖ asset ‖ i64
    amount` (117 bytes). The other five fact kinds carry only their cursor
    and every codec entry returns `NotImplemented` with their task ID.
  - `SlotCertificate` encodes `header_hash ‖ CompactSize bitmap ‖ 96 B
    aggregate`; the bitmap is exactly ⌈k/8⌉ bytes with no bit at or above k.
    `SeatSet::Root` = `Tagged(SEATSET, CompactSize(k) ‖ 48 B keys in index
    order)`.
  - `AccountLeafV0` encodes `account ‖ CompactSize #balances, (asset ‖ i64
    available ‖ i64 reserved)… ‖ CompactSize #used_nonces, u64… ‖ u32
    open_orders`; canonical form: assets strictly ascending with no
    all-zero entry, nonces strictly ascending. R5's accounts tree commits
    to `LeafValue` = `Tagged(STATE_ACCOUNTS, bytes)`.
- Commitment v0 (O-9): sorted-leaf RFC 6962 roots for accounts, orders,
  markets, nullifiers and totals, under a root-of-roots tagged `STATE`. The
  planned sparse Merkle tree is M2-LED-02.
- As landed in R5 (`core/state_root.h`, `ledger/state_commitment.h`; all
  PROVISIONAL REGTEST, O-32): each component is `MerkleRoot` of its leaf
  values in ascending key order, every key once, so the root depends on the
  state alone:

  | Component | Key | Leaf value |
  |---|---|---|
  | accounts | `AccountId` | `Tagged(STATE_ACCOUNTS, AccountLeafV0)` of the canonical leaf |
  | orders | `OrderId` | `Tagged(STATE_ORDERS, RestingOrderLeafV0)`: order ‖ market ‖ owner ‖ u8 side ‖ u64 price ‖ u64 lots ‖ u64 arrival_rank ‖ i64 reserved_left ‖ i64 cum_quote ‖ i64 fee_charged ‖ u64 fills (R7) |
  | markets | `MarketId` | `Tagged(STATE_MARKETS, 80-byte descriptor)` |
  | nullifiers | `Outpoint` | `Tagged(STATE_NULLIFIERS, txid ‖ u32 vout)`, one per credited deposit |
  | totals | `AssetId` | `Tagged(STATE_TOTALS, asset ‖ i64 deposits ‖ i64 effects ‖ i64 pending ‖ i64 fees_held)` |

  `state_root = Tagged(STATE, u8 0 ‖ accounts ‖ orders ‖ markets ‖
  nullifiers ‖ totals)`; the version byte separates v0 from the SMT roots of
  v1. Protocol accounts are `Tagged(PROTOCOL_ACCOUNT, "SEAT_POOL" ‖
  profile_hash)` and `Tagged(PROTOCOL_ACCOUNT, "TREASURY" ‖ profile_hash)`
  (raw ASCII, then the 32 bytes). `ProveAccountV0` gives the leaf and its
  `MerkleProof` under the accounts root; v0 has no absence proof.

**Within-slot order (PROVISIONAL, O-4 and O-4b).** Sort originals by
`Tagged(ORDER, parent_header_hash ‖ height ‖ ActionId)`, then give each
account's positions to its actions in ascending nonce. Plan §3.5 seeds with
`parent_cert_hash`. That lets a leader grind the order, because a BLS
multisignature certificate is not unique: each quorum subset gives a different
bitmap and aggregate. The outline uses the parent **header** hash and needs the
owner's ratification under D4 (M3-ENG-12).

---

## 10. Admission and reads

### 10.1 Admission order (refines plan §3.8 and G5, O-15)

| Step | Check | Code | Signature verified? |
|---|---|---|---|
| A0 | node fenced | `HALTED` | no |
| A1 | frame caps, exact decode | `MALFORMED`, `OVERSIZE` | no |
| A2 | version, domain, kind | `WRONG_DOMAIN`, `UNKNOWN_KIND`, `UNSUPPORTED_KIND` | no |
| A3 | same `ActionId` **and** signature as an action this node already authenticated (pool or retained outcome index) | `DUPLICATE{PendingRef or IncludedRef}` | no |
| A4 | account exists in the head snapshot | `NO_ACCOUNT` | no |
| A5 | nonce not below the window | `NONCE_TOO_LOW` | no |
| A6 | expiry window against head+1, checked arithmetic | `EXPIRED`, `REF_SLOT_AHEAD`, `BAD_EXPIRY` | no |
| A7 | budget | `BUDGET` | no |
| A8 | BIP340 via `VerifiedAction::Verify` | `BAD_SIG` | **yes** |
| A9 | nonce used / pending; same `ActionId` with another valid signature | `NONCE_USED`, `NONCE_PENDING`, `DUPLICATE` | yes |
| A10 | pool insert | `POOL_FULL` or `ADMITTED` | yes |

A4 to A9 use one `HeadView` snapshot. A3 runs before the expiry check so that
an included action resent after its window still returns `DUPLICATE` with its
original outcome (DUR-4, DUR-5), not `EXPIRED`. This satisfies TRD-17
("authenticate before dedup") because A3 only matches byte-identical originals
that were already authenticated (the sigcache pattern).

### 10.2 Read service

- A `SlotView` is immutable: header, certificate, outcomes and their tree, v0
  account leaves and tree levels, an order-fill index and an action index.
- `Publish` requires a `DurableApplied` whose header matches the view. It
  publishes a `CertifiedEnvelope` (header + certificate) into C1 `ReadSlot` with
  `ReadScope{domain, configuration = profile_hash, validator_set =
  seat_set_root, anchor = anchor hash, epoch = authority_epoch,
  authority_revision = anchor height}` and `CommitIdentity{id = header hash,
  parent, state_root, sequence = height}`.
- **One lease per reply.** The `SlotView` is found by the lease's `commit.id`, so
  header, certificate, leaf, proof and scope always belong to one head.
- Labels: `CERTIFIED` means "verified against the trusted seat set". In the
  outline that seat set is the k=1 dev seat. `checkpointed` and `B3-final` do not
  exist yet and are never merged into one label (INV-13).
- `EXPIRED` is returned only with a proof that `VerifyExpired` accepts, taken
  from the same lease; otherwise `UNKNOWN`. `ReadService::Create` refuses an
  outcome index shorter than `max_delay_slots`, so `DUPLICATE` stays definite
  for the whole expiry window.

---

## 11. Engine semantics (all PROVISIONAL, REGTEST profile)

Profile `fm2-dex1-regtest-provisional/1` (`PROFILE_DEX1_REGTEST.json`):
`nonce_window = 100`, `max_delay_slots = 1024`; caps 1,024 actions per slot,
1,024 fills per order, 16,384 fills per slot, 8,192 changed leaves per slot,
1,000 open orders per account, 4,096-byte payload; fee 100 ppm, `SELLER`
incidence, seat share 80%; matching A2; `price_band_enabled = false`,
`futures_enabled = false`, `signer_mode = STRICT`, `pool_capacity = 4096`,
`outcome_index_slots = 1024`, plus `price_band_percent = 50` (unused while
the band is disabled). `DevRegtestProfile` returns
`UNSUPPORTED_NETWORK` for every chain type except REGTEST. As landed in R3
the signer enumerators are `STRICT_SIGNING` and `FOLDED_SIGNING` (registry
names `STRICT` and `FOLDED`, O-24), and `params_tests` fails if the JSON file
and `DevRegtestProfile` differ in any field or in `profile_hash`.

**Validity predicate.** `ValidateSlotInputs` returns `INVALID_INPUT` and no
candidate for: a duplicate `ActionId`; two originals with one (account, nonce);
a domain or profile mismatch; an action outside its window; a nonce excluded by
the parent window; an account absent in the parent state; an unsupported kind;
exceeded action or byte caps; facts not contiguous with the parent's
`facts_to`; an anchor below the parent's anchor.

**Phases** (plan §3.5):

1. Deposits (each outpoint credited once).
2. Transfers: same AssetId, both accounts exist, no fee; self or zero refused
   (O-7).
3. Cancels by `OrderId`.
4. Post-only; a crossing post-only gets `REJ_WOULD_CROSS`.
5. Reserve all takers in canonical order, then one joint `SweepA2`. Same-slot
   proceeds cannot fund same-slot reservations.
6. Rest the GTC residual with `arrival_rank` = canonical rank.
7. Futures: `NotImplemented(M9-FUT-04)`, only reachable if `futures_enabled`.
8. Fees: one `FEES_DISTRIBUTED` outcome per quote asset.
9. `CheckConservation` per asset: Σ(available + reserved) + pending +
   fees_held = deposits − effects (O-31). Failure gives `INVARIANT` and no
   candidate.

Every executed action consumes its nonce, including deterministic rejections
(DUR-5).

**A2 sweep (D4, O-5).** Makers rest in price-time FIFO. At each level the
takers' remaining demand is allocated pro-rata by largest remainder, ties by
canonical rank. Pairing: takers in canonical rank consume makers FIFO
(M3-ENG-14). Execution is at the maker price. Same-slot opposite GTC residuals
that still cross get the certified refusal `REJ_RESIDUAL_CROSS_UNSUPPORTED`
and their reservation is released (M3-ENG-02, waiting for the tie-break
ruling). Self-trade executes normally (O-17, M3-ENG-13).

**Fees (D5, O-6).** The seller pays 100 ppm of matched notional once, in the
quote asset: each fill charges `ceil(cum_quote · ppm / 1e6) − fee_charged` for
that order. Per slot, the seat pool gets ⌊80%⌋ and the treasury gets the rest.
Protocol accounts: `Tagged(PROTOCOL_ACCOUNT, name ‖ profile_hash)`. In the
ledger (R5) the engine settles the quote leg of a fill with
`PayFromReserved(buyer, seller, quote, q, fee_increment)`: the seller
receives q − fee and the fee waits in the asset's `fees_held`; phase 8
`Credit`s ⌊80%⌋ of the slot total to the seat pool and the rest to the
treasury, so `fees_held` is zero at every slot boundary. As landed
in R4 (`core/fee_math.h`): `SellerFeeTotal(q)` = ⌈q · ppm / 10⁶⌉,
`SellerFeeIncrement(q0, q1)` = `SellerFeeTotal(q1) − SellerFeeTotal(q0)`, and
`SplitSlotFees(F, p)` = {⌊F · p / 100⌋, residual}. The walking-skeleton values
(fee 50 on 500,000; split 40/10) are pinned by `fee_math_tests`. The nonce
window and expiry rules are `core/nonce_rule.h` (D7), shared by the ledger,
admission, the engine and b3verify.

**As landed in R6** (`engine/book.h`, `engine/matcher.h`,
`engine/residual.h`; PROVISIONAL REGTEST):

- A resting order holds exactly `RequiredReservation(market, side, price,
  remaining_lots)`: a BUY holds quote atoms at its own price, a SELL holds
  base atoms. The book is never crossed (`Rest` refuses an order that
  would execute), each level is FIFO with strictly increasing
  `arrival_rank`, and fills happen only at the front of the best level at
  that order's price (`FillBestFront` refuses any other quote). R7 must give
  resting orders strictly increasing arrival ranks over the life of a
  market (O-35).
- `SweepA2` takes one market's book, the ledger, that market's takers
  (GTC or IOC, already reserved at their limits), the profile and the
  slot's `SlotFillBudget`. It checks every precondition first and changes
  nothing on a refusal. Then the BUY takers sweep the asks and the SELL
  takers sweep the bids, round by round: the best level's lots are
  allocated over the eligible takers' remaining lots (all of them if they
  fit, else floor shares plus largest remainders, ties to the lower rank),
  and the takers, in ascending rank, consume the level's makers FIFO. A
  taker stops at `max_fills_per_order` fills and its unconsumed share goes
  back to the level for the next round. Each fill takes one unit of the
  budget; at zero, matching stops (O-36).
- Each fill settles at once in the ledger: the seller's reserved base goes
  to the buyer, the buyer's reserved quote goes to the seller less the
  seller's fee increment, which waits in `fees_held`. A maker that runs out
  of lots leaves the book and its owner's open-order count drops. The
  taker's unspent reservation (price improvement, unfilled lots) stays in
  `reserved_left` for phase 6 to release or keep for the resting
  remainder (O-37).
- Every taker ends with a `TakerStop` read from the final book: `FILLED`;
  `NO_CROSS` (nothing reaches its limit); `ORDER_FILL_CAP` or
  `SLOT_FILL_CAP` (the book still crosses). R7 maps them to outcomes.
- Same-slot takers never match each other. `FindResidualCross` names the
  GTC remainders that would rest crossed (a BUY at or above the lowest SELL
  limit, a SELL at or below the highest BUY limit); R7 refuses them with
  `REJ_RESIDUAL_CROSS_UNSUPPORTED`, and the call itself is the stub
  `ResolveResidualCross` (M3-ENG-02; O-38).

**As landed in R7** (`engine/engine.h`; PROVISIONAL REGTEST):

- `ValidateSlotInputs(parent, inputs, profile)` checks, in this order and
  each as `INVALID_INPUT` with a named `SLOT_*` detail: the profile (its
  hash, the parent header's and the parent ledger's), the domain, a
  successor height, the anchor (not below the parent's, the same block at
  the parent's height), the fact range (starts at the parent's
  `facts_to`, not reversed, nothing above the anchor), the caps (facts,
  fact bytes, originals, original bytes) before any decoding, every fact
  (a stub kind returns its R4 `NotImplemented` id; cursors inside the
  range and ascending; deposits the ledger would refuse), every original
  (decode, domain, `KindSupported`, payload, `MaxDelayValid` and the
  expiry window at the slot height, repeated `ActionId`, repeated
  (account, nonce), account in the parent, nonce acceptable in the parent
  window), the ascending insert of each account's nonces, and the BIP340
  signatures last. Its only product is a `ValidatedSlot` (private
  constructor) bound to the parent header hash, holding the decoded
  deposits and actions and the canonical order (O-41).
- `Engine::ApplySlot(parent, slot)` copies the parent state, consumes every
  action's nonce first in canonical order (DUR-5; one account's nonces
  always ascend), runs phases 1-6, 7 (`RunFuturesPhase`, a no-op while
  futures are disabled), 8 and 9, and returns `SlotResult{state,
  outcomes, outcomes_root, roots}`. Outcomes are emitted as: one per
  deposit in body order, one per action in canonical rank, one
  `FEES_DISTRIBUTED` per quote asset with a non-zero fee total, ascending
  `AssetId` (O-42).
- PLACE rejections, in order, with no change but the nonce:
  `REJ_UNKNOWN_MARKET`, `REJ_BAD_PARAMS` (price or lots 0, or a
  reservation above `MAX_ATOMS`: user-controlled overflow is a certified
  rejection, never an error), `REJ_WOULD_CROSS` (post-only),
  `REJ_OPEN_ORDER_CAP` (GTC and post-only take their open-order slot when
  they are placed), `REJ_INSUFFICIENT_AVAILABLE`. A cancel of an order
  that is not resting (never placed, closed, or placed in the same slot)
  is `REJ_UNKNOWN_ORDER`; `REJ_ORDER_CLOSED` would need a closed-order
  index (O-42).
- Phase 6 per taker: `FILLED`; `REJ_FILL_CAP` when either fill cap stopped
  it while the book still crosses (O-40); `IOC_DONE`;
  `REJ_RESIDUAL_CROSS_UNSUPPORTED`; else the remainder rests at its limit
  with exactly its reservation (`RESTING` or `PARTIAL_RESTING`) and the
  price improvement is released. Post-only orders rest in phase 4 with
  `ArrivalRank(height, POST_ONLY, rank)` and remainders in phase 6 with
  `ArrivalRank(height, REST, rank)`, so at one level a post-only order of
  the slot queues ahead of a remainder of the slot (O-39).
- Phase 9 checks `EngineState::CheckInvariants` (conservation, books, no
  fees held, reserved balances equal to the resting orders' reservations,
  open-order counts equal to the resting orders), the profile's fill and
  delta caps per outcome, and that the outcomes' deltas reproduce the
  balance transition from the parent exactly; then the roots. A slot
  never makes more than `max_fills_per_slot` fills over all markets.
- The state commitment is `EngineState::Commit` =
  `CommitTradingStateV0(ledger, every market, every resting order's
  RestingOrderLeafV0)`.

---

## 12. Outline slices

Each slice is one or a few small commits named `flowmesh2: <imperative>`. Each
commit builds and adds tests. TODO.md lists their acceptance criteria.

| Slice | Content | Depends on |
|---|---|---|
| R1 | Build gate, `[[nodiscard]]` error model, Boost.Test runner, inventory and TODO lint | — |
| R2 | Guards (7 rules), compile-fail controls, evidence tool, sanitizer canaries, CI workflow | R1 |
| R3 | Core primitives, profile, authority types, slot header | R1 |
| R4 | Signed actions, slot body, outcomes, facts, certificate, seat set, shared spec functions | R3 |
| R5 | Shared ledger, nonce window, commitment v0 | R4 |
| R6 | Order book, A2 sweep with pairing, reference matcher | R5 |
| R7 | Engine slot phases, canonical order, validity predicate | R6 |
| R8 | Storage: framing, journals, strict commit, tokens, signer and commit logs | R4 |
| R9 | State owner: preview, re-execution, apply, replay | R7, R8 |
| R10 | Chain adapter seam, agreement interfaces, PBFT stubs, dev certifier, fake chain | R9 |
| R11 | Read service on C1 `ReadSlot`, b3verify | R9 |
| R12 | Admission and action pool | R10, R11 |
| R13 | Service lanes, in-process API, mesh interface, product-boundary stubs | R12 |
| R14 | Deterministic simulator and walking skeleton | R13 |

---

## 13. Build and test

### 13.1 Today (after R7)

The commands in 13.2 work. They build every fm2 target (`fm2_all`): the
libraries `fm2_types` and `fm2_core` (since R5; `engine/` since R6,
`engine/engine` since R7), `test_flowmesh2` (250 Boost cases), the unchanged C1 binary and, with the option on, the canaries. They run eight CTests: `flowmesh2_foundations` (C1,
19 cases), `flowmesh2_unit`, `flowmesh2_inventory`, `flowmesh2_todo_lint`,
`flowmesh2_guards`, `flowmesh2_guards_selftest`, `flowmesh2_evidence_selftest`
and `flowmesh2_negative_compile` (Clang). The outline never uses `build/`,
`build-qt/` or `build-nofullfsync/`. Passing `-DBUILD_BENCH=OFF
-DBUILD_FUZZ_BINARY=OFF` only shortens the configure.

### 13.2 Commands (growing with each slice)

```sh
git -C /Users/josh/development/ON/B3-FlowMesh-hl branch --show-current   # must print: rebirth
# An array, so the options split the same way in bash and zsh (zsh does not
# split an unquoted "$COMMON" string; R1's string form broke there).
COMMON=(-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  -DBUILD_TESTS=ON -DBUILD_FLOWMESH2_FOUNDATION_TESTS=ON
  -DBUILD_GUI=OFF -DBUILD_DAEMON=OFF -DBUILD_CLI=OFF -DBUILD_TX=OFF -DBUILD_UTIL=OFF
  -DBUILD_WALLET_TOOL=OFF -DENABLE_WALLET=OFF -DENABLE_IPC=OFF -DWITH_CCACHE=OFF
  -DBUILD_BENCH=OFF -DBUILD_FUZZ_BINARY=OFF)
cmake -S . -B build-rebirth "${COMMON[@]}" -DBUILD_FLOWMESH2=ON
cmake --build build-rebirth --target fm2_all --parallel 4   # every fm2 target (the nm guard needs all of them)
ctest --test-dir build-rebirth -L flowmesh2 --no-tests=error --output-on-failure
```

Isolation and sanitizer checks:

```sh
cmake -S . -B build-rebirth/off  "${COMMON[@]}" -DBUILD_FLOWMESH2=OFF   # no fm2_* target may exist
cmake -S . -B build-rebirth/asan "${COMMON[@]}" -DBUILD_FLOWMESH2=ON \
  -DSANITIZERS=address,undefined -DFLOWMESH2_SANITIZER_CANARIES=ON
cmake -S . -B build-rebirth/tsan "${COMMON[@]}" -DBUILD_FLOWMESH2=ON \
  -DSANITIZERS=thread -DFLOWMESH2_SANITIZER_CANARIES=ON
cmake -S . -B build-rebirth/ubsan "${COMMON[@]}" -DBUILD_FLOWMESH2=ON \
  -DSANITIZERS=undefined -DFLOWMESH2_SANITIZER_CANARIES=ON   # macOS: see O-22
EV=<evidence bundle directory>
cmake --build build-rebirth/ubsan --target fm2_all --parallel 8
python3 -I -B contrib/flowmesh2/evidence.py run --dir "$EV" --label canary_ubsan \
  --env UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 --expect nonzero \
  --expect-output 'runtime error: signed integer overflow' --timeout 120 -- \
  build-rebirth/ubsan/bin/fm2_canary_ubsan
python3 -I -B contrib/flowmesh2/evidence.py run --dir "$EV" --label ctest_ubsan \
  --env UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 -- \
  ctest --test-dir build-rebirth/ubsan -L flowmesh2 --no-tests=error --output-on-failure
python3 -I -B contrib/flowmesh2/evidence.py verify --dir "$EV" --require-expectations
```

`FLOWMESH2_SANITIZER_CANARIES=ON` builds one canary per sanitizer named in
`SANITIZERS` (`fm2_canary_asan`, `fm2_canary_ubsan`, `fm2_canary_tsan`; sources
in `contrib/flowmesh2/canaries/`) and refuses to configure without a
sanitizer. A canary must exit non-zero **with its sanitizer's report**
(`--expect-output`) before any ctest result of that build is trusted; a crash
or a hang without the report does not count. The canaries are not CTests, so
they can never be counted as passing tests. macOS runs are provisional
evidence only; the Linux CI workflow `.github/workflows/flowmesh2.yml` (jobs
`guards`, `asan-ubsan`, `tsan`; first run M0-CI-01) is the gate. On the
current macOS host only UBSan works (O-22). Test temporary directories go
under `build-rebirth/fm2-test-tmp/`.

### 13.3 CTests (label `flowmesh2`)

`flowmesh2_foundations` (C1, still exactly 19 cases), `flowmesh2_unit` (R1),
`flowmesh2_inventory` (R1), `flowmesh2_todo_lint` (R1), `flowmesh2_guards`
(R2), `flowmesh2_guards_selftest` (R2), `flowmesh2_negative_compile` (R2,
Clang only), `flowmesh2_evidence_selftest` (R2), `flowmesh2_sim` (R14),
`flowmesh2_sim_cli_determinism` (R14).

- **Inventory:** `test/INVENTORY.txt` lists every case (`suite/case`, sorted
  bytewise). `check_inventory.py` requires the inventory, the cases the binary
  lists (`--list_content`) and the cases that executed in a full run (Boost XML
  log) to be the same set with equal counts, no disabled or skipped case, and
  exit status 0. Its `--selftest` (always on in the CTest) proves it fails for
  a removed, phantom, duplicated or unsorted inventory line and for a run in
  which a listed case did not execute (`--run_test=!<case>`).
- **TODO lint:** `check_todo.py` checks the TODO.md rules listed in TODO.md
  ("Lint"); its `--selftest` (always on in the CTest) proves each rule fires on
  a doctored copy.
- **Guards** (`check_guards.py`): forbidden includes (`validation.h`,
  `kernel/cs_main.h`, `net_processing.h`, `net.h`, `node/`, `txmempool.h`,
  `index/`, `interfaces/`, `wallet/`, `qt/`, `rpc/`, `init.h`,
  `chainparams.h`, V1 `flowmesh/`); banned constructs (sections 4 and 5);
  `nm` symbol checks (no `cs_main`, `ChainstateManager`, `Chainstate::`,
  `PeerManager`, `CConnman`, `wallet::` or V1 `flowmesh::`; no dev symbols in
  `fm2_core` or `fm2_verify`; no ledger, engine, state or storage symbols in
  `fm2_verify`; no V1 symbols in `test_flowmesh2` or `fm2-sim`); link isolation;
  TODO consistency; the C1 freeze; signing confinement (`bls::SecretKey` and
  `.Sign(` only in `storage/seat_signer.cpp`, `dev/`, `test/`, `sim/`).
  As landed in R2: `--source` runs rules 1, 2, 4, 5, 6 and 7 and `--nm
  BUILD_DIR` runs rules 3 and 4 over `fm2_libs.manifest`; CTest
  `flowmesh2_guards` runs both on the real tree and build. Comments and string
  literals are stripped before the token rules. Rule 5 also requires: the stub
  ID is a literal at the call site; a tag names a row whose status is
  `outline`, or `todo` while none of its slices has landed (never `done`,
  never a `NONE` row); a `STUB`/`REAL-REPLACE` row with a landed slice is not
  `todo`; an `outline` `REAL-REPLACE` row has a tag; a stub_inventory case is
  either `FM2_STUB_CASE(<ID_>, "<ID>")` in C++ test code or a Python method
  `test_stub_<ID_>` in `contrib/flowmesh2/test_*.py`; Python stubs are
  `not_implemented("<ID>")` in `contrib/flowmesh2/*.py`. Rule 6 also refuses a
  later CMake command that modifies a C1 target. Stricter than the slice text
  (O-23): `std::*timed_mutex` and `using namespace std` everywhere;
  floating-point literals, `std::chrono`, `std::thread` and the headers
  `<stdio.h>`, `<ctime>`, `<time.h>`, `<cmath>`, `<math.h>`,
  `<unordered_map>`, `<unordered_set>`, `<future>`, `<condition_variable>`,
  `util/time.h`, `random.h`, `logging.h`, `util/fs.h`, `util/fs_helpers.h` in
  the deterministic directories; `verify/` includes no
  `flowmesh2/{ledger,engine,state,storage}/` header; `->Sign(` is confined like
  `.Sign(`; the C1 library is held to the node-symbol rule. A quoted include
  is checked both relative to its file and on the include path.
  `test_check_guards.py` (CTest `flowmesh2_guards_selftest`) builds a clean
  fake repository from `guard_fixtures/clean/` plus the real C1 files and
  plants one violation per scenario (130 scenarios, including one per list
  entry and real compiled objects for rule 3); each must fail with exactly its
  rule.
- **Negative compile** (`check_negative_compile.py`, Clang only): a missing
  `LOCK`, an ignored `Result` and a forged `Durable*` token each fail to compile
  with the diagnostic named on the file's `FM2_EXPECT_ERROR` line; each file
  built with `FM2_COMPILE_FAIL_FIXED` (the violation removed) compiles, so the
  failure is exactly the violation; a positive control with identical flags
  compiles. The controls are `EXCLUDE_FROM_ALL` object libraries `fm2_cf_*`
  that only this CTest builds (`RUN_SERIAL`).
- **Evidence:** `contrib/flowmesh2/evidence.py run` writes `run.json`, logs and
  `MANIFEST.sha256`; `verify` recomputes the manifest. Stdlib only, run with
  `python3 -I -B`. `run --dir D --label L [--expect zero|nonzero|any]
  [--expect-output RE] [--env N=V] [--timeout S] -- CMD` records one step (a
  label once per bundle) and exits 1 if the expectation is not met; a
  sanitizer canary is recorded with `--expect nonzero`. `verify` fails on a
  changed, deleted or extra file, a symbolic link, a malformed, unsorted,
  duplicated, absolute or escaping manifest entry, or `run.json` log hashes
  that disagree; `--require-expectations` also requires every step to have
  met its expectation. `perf-metadata` is the stub of M0-EVD-01 (exit 3).
  `test_evidence.py` (CTest `flowmesh2_evidence_selftest`) covers all of it.

### 13.4 C1 freeze

The six C1 sources are unchanged since `1f05fc74`. Their SHA-256 values, which
the R2 guard pins (owner-protected):

```
aa3b31a8ec018e420876911e0388bd4926112982bdfa25bef983787240799c56  mailbox.h
a8d9986adb2dc1ebc4e3721900cf761eee47ea41cb378c07f6817d72e2d5a536  mailbox.cpp
fc3e90b50cd3a87caed2eca12f4f3e5a074dd6eab87bfbe00bcf7d3c5397eafd  read_slot.h
702ddde4927299e04cfe743c37106954738527f8b05ce21aa5fe1c5a425ea892  read_slot.cpp
84bd3dfb8f49cb42d1f54506e05cfadd1ef468243ea11363edfdd1294b93104f  foundation_tests.cpp
d53769cee07fdb209ab85284953da0f22904c774b1f409e23f82fbc414e66bfb  read_slot_tests.cpp
```

The C1 block of `src/flowmesh2/CMakeLists.txt` also stays byte-identical.

---

## 14. Walking skeleton (R14 acceptance)

R14 must pass this through both `fm2-sim --scenario <variant> --seed N` and
`sim_walking_skeleton_tests`. Variants: `walking_skeleton`,
`walking_skeleton_storage_fault`, `walking_skeleton_authority_fault`.

### 14.1 Setup

- Profile `fm2-dex1-regtest-provisional/1` on chain type REGTEST.
- `FakeChainAdapter` supplies a synthetic magic and
  `ModernChainDomain = SHA256("fm2/sim/domain")`, both labelled TEST. Neither is
  a real network's value.
- Keys: Alice, Bob and Carol use BIP340 keys `SHA256("fm2/sim/<name>")`. The only
  seat uses BLS IKM `SHA256("fm2/sim/seat0")`, PoP-verified: k = 1, quorum 1,
  bitmap `0x01`.
- Every action uses `ref_slot` = the certified head at signing and
  `max_delay = 16` unless stated.

**Determinism protocol.** The script is a list of steps. Inside a step, the seed
permutes only independent submissions (different accounts, no shared nonce) and
their jitter. A dependent submission is a later step that waits for the earlier
reply (bad-signature c then c; e then e′; c then its resend). `SealSlot` runs
only at scripted boundaries, after `RunUntilIdle`, so slot contents never depend
on the seed.

**Assets.** B3 = AssetId zero, 9 decimals. rUSD = `Tagged(TEST_ASSET, "rUSD")`,
6 decimals. TOK = `Tagged(TEST_ASSET, "TOK")`, 3 decimals.

**Markets** (fixed at genesis; both quote rUSD, which exercises the cross-market
property):

- M1 = B3/rUSD: `base_atoms_per_lot` 1,000,000, `quote_atoms_per_tick` 1.
- M2 = TOK/rUSD: `base_atoms_per_lot` 1, `quote_atoms_per_tick` 10.

### 14.2 Slots

| Slot | Inputs | Expected outcomes |
|---|---|---|
| 1 | Facts (130, 0..2), anchor (130, H130): D1 Alice +1,000,000,000 rUSD (T1:0); D2 Bob +10,000,000,000 B3 (T2:0); D3 Bob +500,000 TOK (T3:0) | 3 × `DEPOSIT_CREDITED` |
| 2 | a) Bob SELL M1 GTC 5,000 lots at 250, n=1. b) Alice BUY M2 GTC 100,000 lots at 600, n=1 | a `RESTING`, reserves 5e9 B3. b `RESTING`, reserves 600,000,000 rUSD |
| 3 | c) Alice BUY M1 IOC 2,000 lots at 260, n=2. d) Alice BUY M2 GTC 100,000 lots at 600, n=3 | c `FILLED`: 2,000 lots at 250 against a; quote 500,000; refund 20,000; Bob's seller fee ceil(500,000·100/1e6) = 50. d `REJ_INSUFFICIENT_AVAILABLE`: 399,480,000 available after c's reservation, 6e8 needed; nonce 3 consumed; no money moves. `FEES_DISTRIBUTED`: seat pool +40, treasury +10 |
| 4 | e) Bob TRANSFER 100,000 rUSD to Alice, n=2. f) Alice CANCEL OrderId(b), n=4. w) Alice BUY M1 GTC 1 lot at 1, n=5, ref_slot 3, max_delay 1 | e `TRANSFERRED`. f `CANCELLED`, releases 6e8. w is admitted but withheld from the proposal by the `FaultPlan` |
| 5 | Heartbeat | the pool gives w a terminal `EXPIRED` |

### 14.3 Final state

Checked twice: from client-verified leaves, and by an independent recomputation
from facts and outcomes.

- Alice: rUSD 999,600,000 available / 0 reserved; B3 2,000,000,000; used
  nonces {1, 2, 3, 4}.
- Bob: B3 5,000,000,000 available + 3,000,000,000 reserved; rUSD 399,950;
  TOK 500,000; used nonces {1, 2}.
- Seat pool: rUSD 40. Treasury: rUSD 10.
- M1 asks: OrderId(a), 3,000 lots at 250. M2 is empty.
- Conservation in every slot: rUSD 1,000,000,000; B3 10,000,000,000;
  TOK 500,000.

### 14.4 Positive checks

- **P1.** Admission returns `ADMITTED` for a-f and w. A golden canonical order is
  pinned for slots 2-4.
- **P2.** Every certificate verifies with b3verify against the trusted seat set:
  `FastAggregateVerify` over `CommitDigest`, bitmap `0x01`.
- **P3.** `TraceRecorder` shows exactly this order for each slot s:
  `authority.check → signer.append(ACCEPTED_PROPOSAL,s) →
  signer.append(INTENT,s) → signer.barrier → authority.check → bls.sign(s) →
  signer.append(SIGNED_VOTE,s) → signer.barrier →
  commit.append(DECISION{header,cert,body},s) → commit.barrier →
  authority.check → state.apply(s) → commit.append(APPLIED,s) →
  commit.barrier → read.publish(s) → pool.on_committed(s)`. One publication per
  slot; none before its `APPLIED` barrier.
- **P4.** The head advances through 1..5. Every `AccountReply`'s scope, header,
  certificate, leaf and proof come from one lease (same `commit.id`), labelled
  `CERTIFIED`.
- **P5.** Alice gets c as `INCLUDED` in slot 3 and verifies the certificate, the
  outcome under `outcomes_root` and her leaf under `state_root`. `VerifyFill`
  recomputes quote 500,000 and fee 50. `VerifyBalanceTransition` from her slot-2
  leaf: rUSD available 400,000,000 → 399,500,000; B3 0 → 2,000,000,000. Bob
  verifies his maker side through `OrderFills(OrderId(a))`: rUSD +499,950;
  B3 reserved −2,000,000,000. (R2 = R1.)
- **P6.** Seeds 1..32 give identical header hashes for slots 1..5 and an
  identical final state root. The same seed twice gives an identical trace
  digest. The CLI output is byte-identical across two runs.
- **P7.** `ReplayDecisions(genesis, commit-journal records)` reproduces headers
  1..5 and the final root. With slot 3's `DECISION` duplicated, slot 3 is applied
  once.
- **P8.** An independent per-slot tally equals `CheckConservation` in every slot.

### 14.5 Negative checks (all definite; none silent)

- **N1 `BAD_SIG`.** Before c, Alice submits c with `sig[0] ^= 1`: `BAD_SIG`,
  `sig_verifications` +1. Later the valid c is `ADMITTED`, so nonce 2 was not
  consumed.
- **N2 `EXPIRED` at admission.** After slot 4, Bob submits a PLACE with n=3,
  ref_slot 1, max_delay 2: `EXPIRED` with no signature verification; his nonce 3
  is not consumed.
- **N3 provable `EXPIRED`.** After slot 5, `GetAction(w)` = `EXPIRED` with a proof
  from one lease. `VerifyExpired(w, header5, cert5, leaf5, proof)` passes:
  5 > 3+1; nonce 5 not in used; |used| = 4 < 100.
- **N4 `DUPLICATE`.** While c is pending its exact bytes give
  `DUPLICATE{PendingRef}`. After slot 3 they give `DUPLICATE{IncludedRef{3, i}}`;
  the returned outcome equals the certified one and verifies under header 3.
  Neither case verifies a signature. No second fill; no balance change.
- **N5 `NONCE_USED`.** After slot 3, a different Alice PLACE with n=2 gets
  `NONCE_USED`, only after its signature is verified.
- **N6 `NONCE_PENDING`.** In the slot-4 window, after e's reply, Bob's e′
  (TRANSFER 1 atom, n=2) gets `NONCE_PENDING`. Only e is included.
- **N7 insufficient funds.** d is a certified `REJ_INSUFFICIENT_AVAILABLE`;
  nonce 3 is in Alice's used set at slot 3; no money moves.
- **N8 `NO_ACCOUNT`.** Carol (n=1, no deposit) signs a PLACE: `NO_ACCOUNT`,
  `sig_verifications` unchanged.
- **N9 `UNSUPPORTED_KIND`.** Alice WITHDRAW with n=6: typed refusal
  (TODO M2-LED-05); nonce 6 not consumed.
- **N10 storage fault** (`walking_skeleton_storage_fault`). The `FaultPlan` fails
  the commit barrier after slot 3's `DECISION` append. `SealSlot` returns sticky
  `STORAGE_FATAL`; no `APPLIED(3)`; the read head stays at 2; `GetAction(c)` =
  `PENDING` exactly; new submits get `HALTED`; the next `SealSlot` returns
  `FENCED` and writes no new `ACCEPTED_PROPOSAL` or `INTENT`.
- **N11 authority change** (`walking_skeleton_authority_fault`).
  `BumpAuthority` between `Preview` and `Propose` gives `AUTHORITY_CHANGED`, no
  signer record, admission fenced. A bump during the intent barrier and a bump
  between decision and apply are unit tests in R9/R10.
- **N12 `WRONG_DOMAIN`.** Alice signs a PLACE (n=7) under another magic's domain:
  `WRONG_DOMAIN` with no signature verification.
- **N13 decision replay.** After slot 4, re-applying slot 3's `DurableDecision`
  and candidate gives `STALE_PARENT`; head and state unchanged.

The checks manifest is {P1-P8, N1-N13} plus the three variants. A check that does
not run counts as a failure; `fm2-sim` exits 0 only if every check ran and
passed. Its JSON has sorted keys and no times, paths or pointers.

---

## 15. Contradictions reported (solve-and-report)

Each item is either a provisional choice the owner must ratify or a conflict the
outline does not resolve on its own.

| ID | Item | Outline handling |
|---|---|---|
| O-1 | Workspace and branch lock in `CLAUDE.md` versus the owner's `rebirth` instruction; [CC] "no new branch"; plan §10.1 names `flowmeshV2-dev` | Gate G-1 (section 0) |
| O-2 | Roles: plan §10.1 says Codex implements and Claude audits | The latest owner instruction governs the outline. Codex's first task is M0-AUD-01, so Claude does not certify its own code. |
| O-3 | The frozen oracle profile (curves, 50 ppm per side, strict sequence) differs from D4/D5/D7 | Differential replay covers only the shared ledger subset (M2-LED-01); an independent checker covers the rest (M3-ENG-05) |
| O-4 | Within-slot merge rule | Sort by seed key, then each account's positions by ascending nonce. PROVISIONAL |
| O-4b | Plan defect: seed by `parent_cert_hash` is grindable | Seed by `parent_header_hash ‖ height ‖ ActionId`. Needs ratification under D4 (M3-ENG-12) |
| O-5 | Pro-rata remainder, pairing and residual-cross tie-break | Largest remainder, ties by rank; takers consume makers FIFO; residual cross is a typed refusal (M3-ENG-02, M3-ENG-14) |
| O-6 | Fee rounding and split | Ceiling on the cumulative per-order fee; ⌊80%⌋ to seats, residual to treasury (D6 dust OPEN) |
| O-7 | TRANSFER semantics | Same AssetId, both accounts exist, no fee; self and zero refused. PROVISIONAL. As landed in R5 the order is `REJ_BAD_PARAMS` (self or zero), then `REJ_NO_RECIPIENT`, then `REJ_INSUFFICIENT_AVAILABLE`; only available funds move |
| O-8 | BIP340 message | The `ActionId` |
| O-9 | Commitment | `commitment_version = 0` (sorted RFC 6962 trees) instead of the planned SMT (M2-LED-02) |
| O-10 | Dev certifier k = 1 is below FN-10's k ≥ 4 | REGTEST-only, lives only in `fm2_dev`, guarded out of `fm2_core` |
| O-11 | Moving BLS into its own `b3_bls` target | Owner-gated (M1-GRD-04) |
| O-12 | Test framework | New code uses Boost.Test; the C1 harness is untouched |
| O-13 | Attribution trailers | None (G-3) |
| O-14 | D3 not landed | Gate G-2 |
| O-15 | Admission order and G5 | Section 10.1 |
| O-16 | Plan §3.7's record list cannot support recovery | Added: body in `DECISION`, `ACCEPTED_PROPOSAL`, `view` in `VoteKey`, reserved view-state records |
| O-17 | Self-trade unspecified | Allowed, PROVISIONAL (M3-ENG-13) |
| O-18 | [CC]: completed milestones freeze before dependent work | The outline is a skeleton and claims no C-milestone exit |
| O-19 | Protected gate files | CI workflow, guard lists, inventory floors, C1 hashes, profile JSON and registries are owner-protected (plan §10.5) |
| O-20 | Header fields | Adds `profile_hash`, `authority_epoch`, `facts_root`; `actions_root` covers the exact original bytes |
| O-21 | The macOS `<math.h>` defines `OVERFLOW` and `DOMAIN` as macros, so an enumerator or constant with either name breaks any translation unit that includes `<cmath>` first | R1 names error code 6 `ARITHMETIC_OVERFLOW` (integer unchanged). The R3 tag registry must not use a bare `DOMAIN` identifier either (the tag string `B3/FM2/DOMAIN` is unaffected). The R1 tests include `<cmath>` before the fm2 headers to catch this |
| O-22 | On the current macOS host (macOS 26.5.2, Apple clang 17.0.0 from the Command Line Tools, SDK 15.5, no Xcode or Homebrew LLVM) the ASan runtime spins forever in `AsanInitInternal` (dyld's `_Block_copy` calls `malloc` during shadow setup, which re-enters ASan init) and the TSan runtime segfaults before `main`, even for a hello-world program. UBSan works | R2 records the ASan and TSan canaries as **not flagged** (hang killed by the watchdog; SIGSEGV without a report) and adds a UBSan-only local build. ASan and TSan evidence must come from the Linux workflow (M0-CI-01) or a newer toolchain; installing one is an owner decision |
| O-23 | The slice text lists the guard patterns; R2 adds stricter ones (section 13.3) | Additions only tighten. The owner may drop any of them before M0-AUD-01; until then they are part of the protected guard lists |
| O-24 | Windows headers define `STRICT` as a macro (`windef.h`), so an enumerator named `STRICT` breaks any translation unit that includes them first | R3 names the signer modes `SignerMode::STRICT_SIGNING` and `FOLDED_SIGNING`; the registry names (`ToString`, the JSON) stay `STRICT` and `FOLDED`. Not compiled on Windows here (no MSVC or MinGW on this host) |
| O-25 | An RFC 9162 inclusion check alone accepts some wrong tree sizes for the same root (index 0 of 4 leaves verifies as a tree of 3), which would let a size-dependent claim (for example "last leaf" in a sorted v0 tree) be forged | R3 adds the tag `MERKLE_ROOT`: the committed root is `Tagged(MERKLE_ROOT, u64 n ‖ MTH)`, like a signed tree head. A mutation that drops the size is caught by `merkle_tests/tampered_proofs_fail`. The owner may prefer an RFC-exact root plus a trusted size per tree; that would change every root vector |
| O-26 | Which hash binds the network | `ProfileHash` covers every rule field and, of the network, only the chain type; the magic and `ModernChainDomain` are bound only by `DomainHash`. The header carries both, so a slot is bound to both. This keeps one `profile_hash` in `PROFILE_DEX1_REGTEST.json` for every regtest network. `DevRegtestProfile` therefore takes a `ChainType`, not a `NetworkId` |
| O-27 | Outline caps that are not profile fields | R4 adds provisional constants: `MAX_SLOT_FACTS` = 1,024 and `MAX_FACT_BYTES` = 1,024 (slot body), `MAX_SEATS` = 1,024 (bitmap ≤ 128 bytes), `MAX_LEAF_ASSETS` = 256 (account leaf), `MAX_WITHDRAW_SCRIPT_BYTES` = 128, and an outcome delta cap of 2 · (`max_fills_per_order` + 1). The owner may move any of them into the profile (which changes `profile_hash` and the JSON) |
| O-28 | `ActionKind` integers | Plan §3.6 lists PLACE, CANCEL, CANCEL_ALL, WITHDRAW, TRANSFER; an earlier draft had TRANSFER = 4, WITHDRAW = 5. R4 follows the plan (WITHDRAW = 4, TRANSFER = 5). The registry is append-only from now on |
| O-29 | Admission needs `UNKNOWN_KIND` and `WRONG_DOMAIN`, but the R4 slice text says the codec refuses an unknown kind | `ActionV2::Decode` refuses both (DECODE) and marks them with the static details `ACTION_UNKNOWN_KIND` and `ACTION_UNKNOWN_VERSION`; the kind byte has a fixed position, so the refusal is decided before parsing. Admission (R12) compares the detail. A dedicated `ErrorCode` would avoid string matching but changes an owner-protected registry |
| O-31 | The R5 slice text gives conservation as Σ(available + reserved) + pending = deposits − effects, but D5 splits a **slot's** fee total (⌊80%⌋ / rest), so a fee charged at a fill must be held somewhere until phase 8 | R5 adds `fees_held` per asset: `PayFromReserved` withholds the seller's fee increment from the quote leg into it, and `Credit` (protocol accounts only) pays out of it. Conservation is Σ(available + reserved) + pending + fees_held = deposits − effects, checked after every operation. The engine must leave `fees_held` at zero after phase 8 (R7) |
| O-32 | Commitment v0 details the plan leaves open | R5 chooses (section 9): leaf values tagged per component and keyed by id; the root-of-roots carries `u8 commitment_version`; a nullifier leaf is the outpoint only; the totals leaf holds deposits, effects, pending and fees_held; all-zero totals and balances are omitted. Every vector is skeleton-only until the SMT (M2-LED-02) |
| O-33 | Where commitment types live: the slice text puts `TreasuryAccount`/`SeatPoolAccount` and the commitment in `ledger/`, but b3verify (R11) must recompute a state root and may not include ledger headers (guard rule 1) | R5 puts the protocol account ids, `AssetTotals`, `TotalsLeafV0`, the market and nullifier leaf values and `StateRootsV0` in `core/state_root` (fm2_types); the ledger builds the trees in `ledger/state_commitment` |
| O-34 | Refusals that are errors, not outcomes | A deposit fact with amount 0 or a protocol account as target, a second fact for one outpoint, deposits of an asset above 2^62 in total, and a 257th asset in one account are errors, so a slot carrying one has no valid candidate. The chain adapter (R10, MB1-ADP-06) must never deliver such a fact and R7/M7-GOV-01 must keep the listed assets at most `MAX_LEAF_ASSETS`; otherwise M2-LED-07 must define certified refusals. The cumulative `deposits`/`effects` counters are capped at 2^62 atoms; tracking net custody instead is for M2-LED-03/M2-LED-05. A TRANSFER to an existing protocol account is allowed (O-7 literal); M7-FEE-01 may forbid it |
| O-30 | Shapes the engine has not pinned yet | `SlotBody` keeps facts as canonical bytes (the body stays decodable without knowing every fact kind; R7's validity predicate decodes them). `OutcomeV2` fields, the delta aggregation rule and the `SLOT` outcome subject are provisional until R6/R7; changing them after R7 changes `outcomes_root` vectors. R6 fills `Fill` as specified (maker, taker, market, taker side, the maker's price, lots, quote, the seller's fee increment and `seller_quote_before`) without changing the codec |
| O-35 | Book invariants the plan leaves open | R6 chooses: every resting order is exactly reserved; the book is never crossed (`Rest` refuses a crossing order, so phase 6 must refuse or resolve a crossing remainder first); each level is FIFO with strictly increasing `arrival_rank`, so the book is a function of its orders and can be rebuilt from order leaves (R7 must assign arrival ranks that increase over the life of a market, for example from height and canonical rank); the book never touches the ledger; `fills` counts every fill of an order, but the per-order fill cap applies only to takers, because a cap on a resting order would block its whole FIFO level. The skeleton has no capacity and no `BOOK_FULL` (M3-ENG-01) |
| O-36 | Fill caps (D6) | A taker stops at `max_fills_per_order` fills; its unconsumed pro-rata share is re-allocated at the same level in the next round. One `SlotFillBudget` (`max_fills_per_slot`) is shared by every sweep of the slot: R7 sweeps markets in ascending `MarketId`, and inside a market the BUY takers sweep before the SELL takers; at zero every sweep stops. The research note's alternative (defer the takers beyond the cap whole to the next slot) contradicts definite outcomes (DUR-6), and the registry has no `SLOT_FULL` outcome code (adding one changes an owner-protected registry), so R7 must map a slot-capped remainder to an existing code. M3-ENG-04 rules both |
| O-37 | What the sweep does and does not do | It settles every fill in the ledger immediately (`PayFromReserved` on both legs, fee withheld into `fees_held`) and closes makers (`RemoveOpenOrder`). It does not release a taker's unspent reservation, rest a remainder, consume nonces or build outcomes; R7 does. Preconditions are checked first and a refusal changes nothing; an error after that means a broken invariant elsewhere (for example another market's reservations that the ledger no longer covers), the state is then unspecified, and R7 discards its candidate (no copy-on-write inside the sweep). Pro-rata demand is each taker's remaining lots, not capped by the fill caps |
| O-38 | Residual-cross and price-band interfaces | `FindResidualCross` treats every GTC remainder that crosses any opposite remainder as crossing (a BUY at or above the lowest SELL limit, a SELL at or below the highest BUY limit), so R7 refuses all of them, not only the max-volume subset a call would execute. `ResolveResidualCross` returns a target `ResidualCall{clearing_price_ticks, buy_lots, sell_lots}`; `PriceBandCheck(market, price, ReferencePrice{market, price_ticks})` returns `Result<bool>`. M3-ENG-02 and M3-ENG-03 may reshape them |
| O-39 | Arrival rank of resting orders: the R7 slice text says "arrival_rank = canonical rank", but post-only orders rest in phase 4 and remainders in phase 6, and the book needs strictly increasing ranks per level over the life of a market (O-35) | `ArrivalRank(height, phase, rank)` = (2·height + (phase = REST ? 1 : 0))·max_actions_per_slot + rank: by slot, then post-only before remainders, then canonical rank. A post-only order of a slot therefore queues ahead of a same-slot remainder at its level even with a higher canonical rank. The stride assumes a fixed `max_actions_per_slot`; a change at a versioned height (M7-GOV-03) must keep ranks increasing. PROVISIONAL |
| O-40 | Codes for refused remainders (O-36 asked R7 to map them) | A taker stopped by the per-order or the per-slot fill cap while the book still crosses gets `REJ_FILL_CAP` (GTC and IOC alike); a GTC remainder that crosses a same-slot opposite remainder gets `REJ_RESIDUAL_CROSS_UNSUPPORTED`. Both refuse the REMAINDER only: the outcome still lists the fills made before and their deltas, so a client must not read these two rejection codes as "nothing happened" (`RefusesRemainderOnly`). Every other rejection moves no money. A `SLOT_FULL` code would change the owner-protected registry; M3-ENG-04 rules it |
| O-41 | Validity predicate clauses beyond the slice text | Added: a successor height; an anchor at the parent's height must be the same block; no fact above the anchor (`facts_to` ≤ (anchor height + 1, 0)); fact cursors inside the range and strictly ascending; deposits the ledger would refuse; malformed facts, originals and payloads; `MaxDelayValid`; the ascending insert of each account's nonces (implied by the parent-window clause, checked so apply cannot fail on it); BIP340 signatures, last. A decoded non-deposit fact kind (unreachable while the R4 codecs are stubs) is `SLOT_FACT_KIND`; each fact task adds its engine branch. Precedence is fixed and tested. M4-AGR-11 ratifies the list against `PROTOCOL.md` |
| O-42 | Engine semantics the plan leaves open | Nonces are consumed before phase 1, in canonical order, because phases reorder an account's actions (a nonce-7 TRANSFER runs before a nonce-6 PLACE) and out-of-order inserts could evict a nonce that is still to come. A cancel cannot reach an order of its own slot (the PLACE runs later): `REJ_UNKNOWN_ORDER`. `REJ_ORDER_CLOSED` is not produced (no closed-order index). A GTC taker takes its open-order slot when reserved, so it can be refused at the cap even if it would fill completely. User-controlled overflow (price × lots) is `REJ_BAD_PARAMS`, never an error. Outcome order: deposits, actions by canonical rank, fee outcomes by ascending quote `AssetId`, only for non-zero fee totals; a fee outcome's account is zero. PROVISIONAL |
| O-43 | Housekeeping found in R7 | `core/slot.h` and `core/state_root.h` both defined `COMMITMENT_VERSION_V0`, so no translation unit could include both; it is now defined once, in `core/slot.h` (failing compile probe in the R7 evidence). The slice text names "outcome codes as listed in layout §D"; no such section exists in any tracked or scratch document, so the R1 `OutcomeCode` registry is used unchanged |

---

## 16. Keeping this file true

- Until a slice lands, the text here is the target contract. After it lands, its
  headers are normative, and any difference is fixed in this file in the same
  commit.
- Registries (codes, record types, tags) are append-only. Removing or renumbering
  an entry needs an owner ruling.
- When a TODO task is completed, its row in the module table (section 2.1) moves
  from "Typed stubs" or "REAL-REPLACE" to "Real".

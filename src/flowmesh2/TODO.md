# FlowMesh V2 clean core: task ledger (`rebirth`)

Date: 2026-10-16. Branch: `rebirth`, based on `18051472`.
This is the complete task list for the new core. Claude builds the outline
(slices R1 to R14). Codex then completes every other task here, and starts by
auditing the outline (M0-AUD-01). The design is in [ARCHITECTURE.md](ARCHITECTURE.md).

Landed outline slices: **R1** (build gate, error model, Boost.Test runner,
inventory and TODO lint), **R2** (guards, compile-fail controls, evidence
tool, sanitizer canaries, CI workflow), **R3** (core primitives, profile,
authority types, slot header), **R4** (signed actions, slot body,
outcomes, facts, certificate, seat set, account leaf, nonce and fee spec
functions), **R5** (shared spot ledger with nonce windows and
conservation, protocol accounts, state commitment v0 and account proofs),
**R6** (order book, A2 sweep with pairing, fees and fill caps,
residual-cross detection, naive reference matcher), **R7** (canonical
slot order, the slot validity predicate, the engine state and its
commitment with the resting-order leaf, and `Engine::ApplySlot` phases 1-6,
8 and 9) and **R8** (record framing, the journal interface, the memory
and file journals, strict commit, durability tokens minted only by
`Journal::Confirm`, the signer and commit logs, the seat signer and the
outbound vote) and **R9** (the core state, committed heads, slot
candidates with a canonical body, the single-writer state owner with
preview, re-execution and apply, and decision replay) and **R10** (the
chain adapter seam with its batch checks, the capped agreement messages,
the agreement and safety interfaces, and, in `fm2_dev`, the REGTEST fake
chain adapter and the k = 1 dev certifier) and **R11** (immutable slot
views and the read service over the C1 read slot, one lease per reply,
and b3verify in `fm2_verify`) and **R12** (admission A0..A10, the bounded
action pool, the budget policy and, in `fm2_dev`, the unlimited dev
budget) and **R13** (the service lanes with their reserved share for
cancel, status and health, the client API contract and the in-process
API, the seat mesh interface, and the bridge, futures and governance
boundaries as typed stubs) and **R14** (the deterministic simulator: the
fake clock, the seeded scheduler, the fault plan, the trace recorder, the
simulated node and client, and the walking skeleton with its checks
manifest and the `fm2-sim` CLI). With R14 every outline slice has landed;
the next task is M0-AUD-01.
Their rows are `outline` until M0-AUD-01 accepts them, and so are the rows
they tag: M0-EVD-01 (R2); M3-ENG-03, M3-ENG-08, M9-FUT-01, M4-STO-05 and the
REAL-REPLACE M3-ENG-11 (R3); M4-ADM-01, M7-SEAT-01, M7-GOV-01, MB1-B3D-02,
M8-BRG-02, M8-EXIT-01 and the REAL-REPLACE M2-ADM-01 (R4); M2-LED-02,
M2-LED-03, M2-LED-04, M2-LED-05, M2-LED-06 and the REAL-REPLACE M2-LED-07 and
M7-FEE-01 (R5); M3-ENG-02 and the REAL-REPLACE M3-ENG-01, M3-ENG-04,
M3-ENG-13 and M3-ENG-14 (R6; M3-ENG-03 gains its second stub site there);
M3-ENG-09, M9-FUT-04 and the REAL-REPLACE M3-ENG-12 and M4-AGR-11 (R7;
M2-LED-05, M2-LED-06, M3-ENG-02 and M3-ENG-04 gain R7 sites there);
M4-STO-01, M4-STO-02, M4-STO-03, M4-STO-06 and M4-STO-07 (R8; M4-STO-05
gains its second stub site there); M4-AGR-10 and the REAL-REPLACE
M2-LED-10 (R9; M4-STO-03 and M4-STO-07 gain R9 sites there); M4-AGR-01,
M4-AGR-02, M4-AGR-03, M4-AGR-04, M4-AGR-06, M4-AGR-07, MB1-ADP-06 and
M7-ADP-01 (R10); M5-VFY-03, M7-ADP-02, M8-CKP-01 and the REAL-REPLACE
M5-RD-01 (R11; M2-LED-02 gains its R11 site there); M4-ADM-02 (R12;
M4-ADM-01, M2-LED-05, M2-LED-06, M3-ENG-09 and the REAL-REPLACE M2-ADM-01
gain R12 sites there); M5-API-01, M5-API-02, M4-SIM-01, M4-NET-01,
M4-NET-02, M4-NET-03, M8-BRG-01, M8-BRG-03, M9-FUT-02, M9-FUT-03, M7-GOV-02
and M7-GOV-03 (R13; M7-GOV-01, M8-BRG-02, M8-CKP-01, M8-EXIT-01, M9-FUT-01
and M9-FUT-04 gain R13 sites there); M4-SIM-02, M4-SIM-03 and the
REAL-REPLACE M0-SIM-01 (R14; M4-SIM-01 gains its R14 site there, and the
walking skeleton's check N9 tags M2-LED-05).
Every other row is `todo`, except the C1 baseline, which is `done`.

---

## How to read and maintain the ledger

**Row format.** One table row per task, ten columns:

`| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |`

| Column | Meaning |
|---|---|
| ID | Unique. Pattern `R<n>`, `M<n>-<AREA>-<nn>`, `MB1-<AREA>-<nn>` or `B0-<AREA>-<nn>`. The same ID is used in source tags `// TODO(rebirth:<ID>)` and in `stub_inventory` test cases. |
| Ms | Milestone: `R` (outline slice), `M0`..`M11`, `MB1`, `B0`. Mapping to [CC] is at the end. |
| Status | `todo`: nothing in the tree beyond this ledger. `outline`: the slice in column "Slice" has landed and the tagged stub or skeleton exists; Codex completes it. `done`: acceptance met, evidence recorded, tags and `stub_inventory` case removed, reviewed. |
| Skeleton | What the outline leaves in the tree. `STUB`: a typed `NotImplemented`/`Unsupported` error or certified typed refusal, tagged. `REAL-REPLACE`: working, deliberately simple code, tagged, that this task replaces. `NONE`: new work, no tag. `—` for outline slices. |
| Slice | The outline slice that adds the tag (`—` if none). |
| Replaces / location | Where the stub or skeleton lives, or where new work goes. |
| Acceptance | The test that must fail first and then pass. |
| Evidence | Path under the evidence root `$EV = /Users/josh/development/ON/B3-FlowMesh-200ms-evidence/`, one directory per run: `$EV/<Ms>/<ID>/<run-id>/` with `run.json`, logs and `MANIFEST.sha256` (plan §10.2). For outline slices, `scratch:R<n>/` means the provisional session scratch directory `.../scratchpad/rebirth/evidence/R<n>/`; it is not durable, and M0-AUD-01 re-creates the evidence under `$EV`. |
| Refs | Requirement IDs, plan IDs, decisions (D1..D30), reported contradictions (O-n), gates. |

**Status transitions.** When a slice lands, the same commit moves its own R row
and every row it tags from `todo` to `outline`. An R row moves to `done` when
M0-AUD-01 accepts the slice. Any other task moves to `done` only with its
evidence path filled and the owner's or auditor's acceptance.

**Lint.** From R1 on, `contrib/flowmesh2/check_todo.py` (CTest
`flowmesh2_todo_lint`) checks: ten columns; unique IDs matching the pattern
(the same pattern the C++ `TaskId` accepts at compile time); Ms matches the ID;
Status in {`todo`, `outline`, `done`}; Skeleton in {`STUB`, `REAL-REPLACE`,
`NONE`, `—`}; R rows use `—` and name themselves in Slice, and R1..Rn have no
gap; every `STUB`/`REAL-REPLACE` row names existing slices and a location, and
is `outline` only if one of its slices has landed; `NONE` rows have Slice `—`
and are never `outline`; Task, Acceptance and Evidence are not empty. Its
`--selftest` proves each rule fires on a doctored copy.
From R2 on, `check_guards.py` rule 5 (CTest `flowmesh2_guards`) checks that
every `NotImplemented`/`Unsupported` site (with a literal ID) and every
refusal marker has a `TODO(rebirth:<ID>)` within 3 lines, that every tag names
a row here, that every `STUB` row with status `outline` has at least one tag
and one `stub_inventory` case, and that every `REAL-REPLACE` row with status
`outline` has a tag. A tag may name an `outline` row, or a `todo` row while
none of its slices has landed (work in progress inside a slice); never a
`done` or `NONE` row. A `STUB`/`REAL-REPLACE` row whose slice has landed is
not `todo`. A stub_inventory case is `FM2_STUB_CASE(<ID_>, "<ID>")` in C++
test code or, for a stub in a contrib script, a Python test method
`test_stub_<ID_>` in `contrib/flowmesh2/test_*.py`.

**Rules for Codex.**

1. Start every task with a failing test (unit, vector, simulation seed or fuzz
   input). Keep the failing run in the evidence.
2. One logical change per commit, message `flowmesh2: <imperative>`. Every
   commit builds and passes `ctest -L flowmesh2`. No attribution trailers.
   Never push. Never rewrite history.
3. When a `STUB` is finished, delete its tag and its `stub_inventory` case in
   the same commit, and move the module in ARCHITECTURE.md §2.1 to "Real".
4. An outline slice is never a [CC] milestone exit. Each C-milestone freezes for
   review before dependent work starts.
5. **Protected gate files** are owner-controlled (plan §10.5): the CI workflow,
   `INVENTORY.txt` floors, guard lists, the C1 hashes, `PROFILE_DEX1_REGTEST.json`
   and the append-only registries (codes, record types, tags). You may add
   tests. Removing or weakening any of these needs an audit and owner sign-off.
6. `OWNER-GATED`: needs an owner ruling or authorization first (b3d changes,
   shared CMake outside `src/flowmesh2/`, consensus, a new dependency, an OPEN
   parameter). `BLOCKED-ON-RULING`: names the decision it waits for. Do not pick
   a value to unblock yourself; report it.
7. No mainnet contact, no real datadir, no real keys. Network-touching work is
   regtest with no peers, or unit tests.

---

## Provisional decisions D1 to D9 (plan §12.1, Tier 1)

The owner instruction of 2026-10-16 lets the outline proceed with the plan's
recommended defaults, **provisionally**. None of them is valid for mainnet until
the owner ratifies it. Code that depends on them is labelled PROVISIONAL and
lives only in the REGTEST profile `fm2-dex1-regtest-provisional/1`.

| D | Decision | Used by the outline | Where it shows | Ratify / replace in |
|---|---|---|---|---|
| D1 | New core on the same B3 chain (Path A) | **PROVISIONAL yes.** The chain adapter keeps Path B possible. `DomainHash` binds `ModernChainDomain`, `fm_version` and the magic. Nothing touches `b3d`. | `chain/`, `core/params` | MB1, M7-ADP-01 |
| D2 | SEC-1 re-rule (2/3 of seats can move value up to limits) | **Not used.** The outline has no custody code; `VaultV2`/`CheckpointV2` are stubs. | — | before MB1, M8 |
| D3 | C-1 reconciliation: stable quote and fees in Q | **Not landed (gate G-2, O-14).** Fee code stays REGTEST-provisional. | `core/fee_math`, `engine/` | owner commit; then M3-ENG-08 |
| D4 | Matching A2 | **PROVISIONAL.** Makers price-time FIFO; same-slot takers pro-rata per level by largest remainder, ties by canonical rank; takers consume makers FIFO (O-5); residual cross is the refusal `REJ_RESIDUAL_CROSS_UNSUPPORTED`; order seed by parent **header** hash (O-4b, deviates from plan text). | `engine/matcher`, `engine/engine` | M3-ENG-02, M3-ENG-07, M3-ENG-12, M3-ENG-14 |
| D5 | Fee incidence | **PROVISIONAL:** seller pays 100 ppm once in the quote asset, ceiling on the cumulative per-order fee; ⌊80%⌋ seats, rest treasury (O-6). `PER_SIDE` is a stub. The ledger withholds the fee on the quote leg into `fees_held` until phase 8 credits it (O-31). | `core/fee_math`, `core/params`, `core/state_root`, `ledger/ledger` | M3-ENG-08, M7-FEE-01 |
| D6 | Lot/tick and engine caps | **PROVISIONAL:** per-market integer tick and lot fixed at genesis; caps 1,024 actions/slot, 1,024 fills/order, 16,384 fills/slot, 8,192 changed leaves/slot, 1,000 open orders/account, 4,096-byte payload; ±50% band disabled (stub); dust residual to treasury (OPEN). | `core/params`, `core/market`, `engine/` | M3-ENG-03, M3-ENG-04, M3-ENG-11 |
| D7 | Nonce window and expiry | **PROVISIONAL:** window 100; `max_delay ≤ 1,024` slots; cancel by `OrderId`; exact-byte resend allowed; `EXPIRED` provable; admission order refined (O-15). | `core/nonce_rule`, `ledger/`, `admission/`, `verify/` | M2-ADM-01, M2-ADM-03 |
| D8 | Regtest exchange (DEX-1) before finality V2 | **PROVISIONAL yes.** Trusted facts only through `FakeChainAdapter` (REGTEST-only, `fm2_dev`). The b3d adapters are stubs. | `dev/`, `chain/` | MB1-ADP-06, M6-DEX-01 |
| D9 | Dev tools (Quint/Apalache, LazyFS, AFL++) and CI hardware | **Not approved.** The outline uses only in-tree compilers, sanitizers and Python stdlib. | — | M0-CI-02, M0-SPEC-01, M4-STO-04 |

Owner rulings and gates referenced below (not Codex tasks): G-1/O-1 workspace
and `CLAUDE.md` reconciliation; G-2/O-14 D3; O-3 oracle split; O-4/O-4b/O-5
(D4) merge rule, seed source, pairing, tie-break; O-6 (D6) dust; O-7 TRANSFER;
O-15 admission order and G5; O-16 journal record set; O-17 self-trade; the
price-band reference source; D9; Tier 2 D10-D17; Tier 3 D18-D30 and D-G1; C-12
for M10. ARCHITECTURE.md §0 and §15 describe each.

---

## R — outline slices (Claude)

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| R1 | R | outline | — | R1 | Build gate `BUILD_FLOWMESH2`, `[[nodiscard]]` `Result`/`ErrorCode`, `OutcomeCode`/`AdmissionCode`, Boost.Test runner, inventory and TODO lint (these docs landed first) | top-level `CMakeLists.txt` (4 lines); `src/flowmesh2/CMakeLists.txt`; `core/result`, `core/codes`; `test/`; `contrib/flowmesh2/check_inventory.py`, `check_todo.py` | `Result<int>`/`Result<void>` not default-constructible (static_assert); `NotImplemented` carries the exact ID; every enum integer pinned; both lint scripts fail on doctored copies, including a listed case that did not run; OFF configure has no `fm2_*` target; ON without the foundation option fails to configure; C1 still 19 cases; `ctest -L flowmesh2 --no-tests=error` green | scratch:R1/ | G-1, G-2, G-4 |
| R2 | R | outline | — | R2 | Guards (7 rules) with self-tests, compile-fail controls, `evidence.py`, sanitizer canaries, CI workflow | `contrib/flowmesh2/` (`check_guards.py`, `test_check_guards.py`, `guard_fixtures/`, `check_negative_compile.py`, `evidence.py`, `test_evidence.py`, `canaries/`); `test/compile_fail/`; `storage/durable.h` (token type only); `src/flowmesh2/CMakeLists.txt` (`fm2_all`, `fm2_libs.manifest`, `FLOWMESH2_SANITIZER_CANARIES`); `.github/workflows/flowmesh2.yml` | each rule's planted fixture fails and a clean one passes; guards pass on the tree (`--source`, `--nm build-rebirth`); missing `LOCK`, ignored `Result` and forged token fail to compile, positive control compiles; `evidence verify` fails on a tampered, deleted or extra file; macOS ASan, UBSan and TSan canaries exit non-zero | scratch:R2/ | SEC-8, plan §4, O-19 |
| R3 | R | outline | — | R3 | Core primitives, profile, authority types, slot header | `core/ids`, `amount`, `hashes`, `codec`, `params`, `authority`, `market`, `merkle`, `slot`; `PROFILE_DEX1_REGTEST.json` | `AccountId` golden; overflow refused at `MAX_ATOMS` edges and for negatives; `MulDiv` exhaustive on a small domain; tags unique, prefixed, golden, no collision with existing `B3/` tags; trailing bytes, oversize counts (no allocation) and non-canonical `CompactSize` refused; MAIN/TESTNET/TESTNET4/SIGNET give `UNSUPPORTED_NETWORK`; JSON `profile_hash` equals code; Merkle sizes 0..17 equal a naive reference, tampering fails, duplicated last leaf changes the root; header round trip, `Hash ≠ CommitDigest`, golden, every field changes `Hash` | scratch:R3/ | D6, D7, O-9, O-20, O-24, O-25, O-26 |
| R4 | R | outline | — | R4 | Signed actions, slot body, outcomes, facts, certificate, seat set, shared spec functions | `core/action`, `body`, `outcome`, `facts`, `certificate`, `seat_set`, `account_leaf`, `nonce_rule`, `fee_math`; `test/util/keys.h` | BIP340 round trip; flipped bit and other domain fail; `Id` excludes the signature; payload 4,096 ok, 4,097 `OVERSIZE`; `max_delay` 0 and 1,025 flagged; golden bytes and `Id`; `VerifiedAction` only via `Verify` (static_assert); body of 1,025 originals refused; two signatures give two `ActionLeaf`s; bitmap golden, bits ≥ k refused; `SeatSet` refuses bad PoP and duplicates; quorum table k=1..10; nonce rule exhaustive for W=3; fee increments sum to ceil(total·ppm/1e6); split exhaustive F=0..10,000 | scratch:R4/ | D5, D7, O-8, TRD-17 |
| R5 | R | outline | — | R5 | Shared ledger, nonce window, commitment v0 | `ledger/nonce_window`, `ledger`, `state_commitment`; `core/state_root` (commitment v0 value types and protocol accounts, shared with b3verify, O-33); `test/util/ledger_fixtures.h` | W=3 insert sequences of length ≤ 7 equal a naive model; W=100 boundaries (`NONCE_TOO_LOW`, `NONCE_USED`, eviction); duplicate outpoint refused; over-reserve leaves state unchanged; transfer refusals; 64 seeds × 10,000 random ops keep conservation equal to an independent tally; root deterministic and insertion-order independent; proofs verify | scratch:R5/ | TRD-10, O-7, O-31, O-32, O-33, O-34 |
| R6 | R | outline | — | R6 | Order book, A2 sweep with pairing, reference matcher | `engine/book`, `matcher`, `residual`; `test/reference_matcher.h`; `test/util/engine_fixtures.h` | full and partial fill at the maker price, FIFO; 3 takers × 4 lots against 10 maker lots split 4/3/3 with a golden pairing vector; multi-level sweep; `FILL_CAP` at cap−1/cap/cap+1; self-trade conserves and charges once; 1,000 seeded batches equal a naive O(n²) matcher on fills, pairing and fees | scratch:R6/ | D4, D5, O-5, O-17, O-35, O-36, O-37, O-38 |
| R7 | R | outline | — | R7 | Engine slot phases, canonical order, validity predicate | `engine/engine`; `core/state_root` `RestingOrderLeafV0`; orders and markets in `ledger/state_commitment` (`CommitTradingStateV0`); `test/util/slot_fixtures.h` | shared-quote reservation in M2 blocks an M1 order; same-slot proceeds cannot fund a same-slot reservation; transfer, cancel, post-only, IOC, refund cases; deterministic rejection consumes the nonce with zero deltas; each `ValidateSlotInputs` clause has a failing input; permuted originals give identical roots; parent hash changes cross-account order only; golden canonical order; residual-cross refusal releases and conserves; 24 seeds × 16 random slots keep every balance equal to parent + outcome deltas | scratch:R7/ | D4, DUR-5, O-4, O-4b, O-39, O-40, O-41, O-42, O-43 |
| R8 | R | outline | — | R8 | Storage: framing, journals, strict commit, tokens, signer and commit logs | `storage/` (`record`, `journal`, `memory_journal`, `file_journal`, `journal_commit`, `tokens.h`, `durable.h`, `signer_log`, `seat_signer`, `commit_log`, `log_writer`, `snapshot_store`); `agreement/outbound.h`; `test/util/storage_fixtures.h`; `test/compile_fail/sign_without_intent.cpp` | role mismatch `ORDER_VIOLATION`; failed barrier gives sticky `STORAGE_FATAL`, then `FENCED`, `Confirm` refused; `Confirm` before `Barrier` refused; 3 appends + 1 barrier give 3 confirms; equivocation refused, identical digest idempotent, new view allowed; `Sign` without `DurableIntent` does not compile; `AppendApplied` wrong root `VERIFY_FAILED`; framing golden; CRC, chain and splice errors detected; file journal refuses a non-empty dir, reads back zero-filled at full size, scan reproduces records, a forced commit failure is sticky | scratch:R8/ | DUR-1, DUR-2, DUR-8, O-16, O-44, O-45, O-46, O-47, O-48, O-49 |
| R9 | R | outline | — | R9 | State owner: preview, re-execution, apply, replay | `state/candidate`, `state_owner`, `replay`; `storage/tokens.h` and `storage/commit_log` (the decision token names its record's certificate, O-52); `test/util/state_fixtures.h` | `Preview` leaves the head unchanged and is byte-identical for identical inputs; permuted originals give the same header; `STALE_PARENT`; token for another header `VERIFY_FAILED`; superseded epoch `AUTHORITY_CHANGED` with head unchanged; same decision twice `STALE_PARENT`; `Reexecute` rejects one flipped root; five-slot chain links; `ReplayDecisions` over a `MemoryJournal` reproduces headers and root, duplicated `DECISION` applied once; injected conservation failure `INVARIANT`; each leader-fault input `INVALID_INPUT` | scratch:R9/ | DUR-1, CONS-10, O-50, O-51, O-52, O-53, O-54, O-55, O-56 |
| R10 | R | outline | — | R10 | Chain adapter seam, agreement interfaces, PBFT stubs, dev certifier, fake chain | `chain/` (`chain_adapter`, `b3d_adapter`); `agreement/` (`messages`, `agreement.h`, `safety.h`, `pbft`, `leader`, `inclusion`, `restart_fence`, `evidence`); `dev/dev_certifier`, `dev/fake_chain` (`fm2_dev`); `test/util/agreement_fixtures.h`, `test/util/require.h` | message round trip, caps, golden; one slot writes exactly `ACCEPTED_PROPOSAL`, `INTENT`, `SIGNED_VOTE`, `DECISION` in barrier order; certificate passes `FastAggregateVerify` over `CommitDigest`; non-REGTEST and k=2 refused; authority bump before `Propose` writes nothing; bump during the intent barrier leaves a durable `INTENT`, no signature, other digest refused; failed vote barrier gives `STORAGE_FATAL` and no `DECISION`; fake adapter cursor, `BUSY`, `NEED_DATA`, rewind, anchor ancestry; no dev symbols in `fm2_core` | scratch:R10/ | D8, O-10, CONS-10, O-57, O-58, O-59, O-60, O-61, O-62 |
| R11 | R | outline | — | R11 | Read service on C1 `ReadSlot`, b3verify | `read/views`, `read_service`; `verify/b3verify` (library `fm2_verify`); `core/merkle` `MerkleTree`; `test/util/verify_fixtures.h`, `read_fixtures.h`; guard rules 1 and 3 widened (`contrib/flowmesh2/check_guards.py`, self-test) | mismatched token refused; 4 readers + 1 publisher × 2,000 iterations: every reply from one head; eviction then `UNKNOWN`; outcome index below `max_delay` refused at `Create`; maker fill lookup; `VerifyCertificate` passes and fails for wrong seat set, tampered header, below-quorum bitmap, bit ≥ k, other-subset aggregate; tampered outcome proof, wrong fee or delta fail; `VerifyExpired` negatives; `fm2_verify` has no `fm2_core` symbols | scratch:R11/ | SEC-5, INV-13, DUR-5, O-63, O-64, O-65, O-66, O-67, O-68 |
| R12 | R | outline | — | R12 | Admission and action pool | `admission/admission`, `pool`, `budget`; `dev/dev_budget` (fm2_dev); `test/util/admission_fixtures.h` | every code once with counters: no signature check before A8, `BAD_SIG` adds 1, `NONCE_USED`/`NONCE_PENDING` only after a valid signature; precedence table; included action resent after its window gives `DUPLICATE`, not `EXPIRED`; `REF_SLOT_AHEAD` near `UINT64_MAX` without overflow; pool `IN_PROPOSAL`, expiry, capacity, withhold filter, commit removal, same-nonce loser terminal `NONCE_USED` | scratch:R12/ | PLAN-9, DUR-4, DUR-6, O-15, O-69, O-70, O-71, O-72, O-73, O-74 |
| R13 | R | outline | — | R13 | Service lanes, in-process API, mesh interface, product-boundary stubs | `net/` (`lanes`, `client_api`, `in_process_api`, `https_sse_api`, `seat_mesh`, `in_process_mesh`, `tcp_mesh`, `short_id`, `sentry`); `bridge/` (`bridge_manager`, `checkpoint`, `forced_exit`); `futures/futures`; `governance/governance`; `test/util/api_fixtures.h`, `mesh_fixtures.h` | lane priority; cancel and status accepted while the ACTION lane is full; `BUSY` leaves no queued copy; 4 submit threads + pump + publisher give exactly one reply per request ID; `GetAction` never returns `EXPIRED` without a verifying proof; every stub returns its ID | scratch:R13/ | SEC-3, SEC-4, UX-7, DUR-6, PERF-12, O-75, O-76, O-77, O-78, O-79, O-80 |
| R14 | R | outline | — | R14 | Deterministic simulator and walking skeleton | `sim/` (`clock.h`, `scheduler`, `faults.h`, `trace`, `sim_node`, `sim_client`, `cluster`, `scenario`, `fm2_sim_main.cpp`; library `fm2_simlib`, CLI `fm2_sim_cli` = `fm2-sim`); `storage/seat_signer` (`SignObserver`, O-81); `contrib/flowmesh2/sim_determinism.cmake`; CTests `flowmesh2_sim`, `flowmesh2_sim_cli_determinism` | ARCHITECTURE.md §14: P1-P8 and N1-N13 for seeds 1..32 and the three variants; scheduler tie orders for seeds 1 and 2 match goldens; same seed twice gives the same trace digest; CLI output byte-identical across two runs; full `ctest -L flowmesh2` green | scratch:R14/ | PERF-6 (no latency claim), O-81, O-82, O-83, O-84, O-85, O-86, O-87, O-88 |

## M0 — verification foundation (plan addition)

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M0-AUD-01 | M0 | todo | NONE | — | Audit the R1..R14 outline on a clean checkout: interfaces, stubs, PROVISIONAL labels, guards, negative compile, inventory; re-create durable evidence for each slice | whole outline | audit report with a verdict per slice (met / partly / not met) and evidence links; every guard, canary and negative control re-run and seen to fire | M0/M0-AUD-01/ | OPS-4, O-2 |
| M0-CI-01 | M0 | todo | NONE | — | Run `flowmesh2.yml` on Linux; with the owner, settle the review's workflow notes (the file is owner-protected): the push `paths:` filter leaves out `src/CMakeLists.txt` (which adds `src/flowmesh2`) and the reused Bitcoin sources (hash, pubkey, sync, crypto, util); every job is gated on `refs/heads/rebirth`, so other branches and pull requests get no run; actions are pinned by tag, not commit SHA; ctest output lands only in the evidence artifact | R2 workflow | canaries fail; suites pass; `evidence.py verify` OK; each note ruled or changed with the owner's sign-off | M0/M0-CI-01/ | SEC-8, OWNER-GATED |
| M0-CI-02 | M0 | todo | NONE | — | Nightly runs on a ≥ 32-core Linux PLP box | new | ≥ 256 CPU-h DST per night; `nr_throttled = 0` | M0/M0-CI-02/ | D9, OWNER-GATED |
| M0-CI-03 | M0 | todo | NONE | — | `DEBUG_LOCKORDER` profile; decide whether to enable `-Wthread-safety-negative`, without which the `EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)` annotations are not checked at call sites | new | a planted lock inversion is detected; a planted call with the lock held is caught, or the decision not to enable the flag is recorded | M0/M0-CI-03/ | SEC-8 |
| M0-FUZ-01 | M0 | todo | NONE | — | `FUZZ_TARGET` for every fm2 decoder: `ActionV2`, `SlotBody`, `SlotHeaderV2`, `OutcomeV2`, `AccountLeafV0`, `SlotCertificate`, agreement messages, journal records, `FactBatch` | new | 60 s smoke each; a planted decoder bug is found | M0/M0-FUZ-01/ | plan L5 |
| M0-PRP-01 | M0 | todo | NONE | — | Property catalogue v1 plus a guard that every P-ID has a test | new | the guard fails when a test is missing | M0/M0-PRP-01/ | plan M0 |
| M0-GUIX-01 | M0 | todo | NONE | — | Guix build of the fm2 binaries on 2 hosts | new | identical hashes | M0/M0-GUIX-01/ | SEC-6 |
| M0-SIM-01 | M0 | outline | REAL-REPLACE | R14 | Record/replay and trace minimizer | `sim/scheduler.cpp` (`Scheduler::At` draws the seeded tie of a plain `std::map` event queue; R14) | 1,000 seeds replay byte-identical; a planted failure is minimized | M0/M0-SIM-01/ | plan M0 |
| M0-SPEC-01 | M0 | todo | NONE | — | Quint/Apalache toolchain and PBFT spec skeleton | new | the spec check runs in CI | M0/M0-SPEC-01/ | D9, OWNER-GATED |
| M0-EVD-01 | M0 | outline | STUB | R2 | PERF-7 metadata: fsync probe, RTT matrix, `nr_throttled`, B3-advanced flag, topology | `evidence.py perf-metadata` (exits 3) | `verify` fails when a field is missing | M0/M0-EVD-01/ | PERF-6, PERF-7 |

## M1 — [CC] C1 plus guard addendum

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M1-C1-00 | M1 | done | NONE | — | C1 foundations: `BoundedMailbox`, `ReadSlot` | `mailbox.*`, `read_slot.*` | 19 cases, no skips; lock-annotation negative control | `src/flowmesh2/C1_RESULTS.md` (`1f05fc74`) | [CC] C1 |
| M1-GRD-01 | M1 | todo | NONE | — | `nm` guard in Linux CI | R2 rule 3 | a scratch `cs_main` reference fails CI | M1/M1-GRD-01/ | plan §4 |
| M1-GRD-02 | M1 | todo | NONE | — | GCC build job; Clang stays the thread-safety gate | new | both jobs green | M1/M1-GRD-02/ | plan §5 |
| M1-GRD-03 | M1 | todo | NONE | — | CI asserts TSan has no suppressions | R2 workflow | a suppression file makes the job fail | M1/M1-GRD-03/ | plan §4 |
| M1-GRD-04 | M1 | todo | NONE | — | Move BLS into its own `b3_bls` target | `src/crypto/CMakeLists.txt` | node unchanged; fm2 links `b3_bls` | M1/M1-GRD-04/ | O-11, OWNER-GATED |
| M1-GRD-05 | M1 | todo | NONE | — | C1 hash pins; changes need owner sign-off | R2 rule 6 | an unpinned change fails | M1/M1-GRD-05/ | [CC] C1, O-19 |
| M1-GRD-06 | M1 | todo | NONE | — | Negative-compile CTests in Linux CI | R2 `compile_fail/` | each control fires on Linux Clang | M1/M1-GRD-06/ | plan M1 |

## M2 — [CC] C2 shared application

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M2-LED-01 | M2 | todo | NONE | — | Differential replay against the frozen model on the shared ledger subset; a vector exporter reads the frozen model, a C++ runner replays | new | ≥ 10^6 actions × 1,000 seeds byte-identical on Linux, macOS and Windows; excluded semantics listed | M2/M2-LED-01/ | [CC] C2, O-3, OWNER-GATED |
| M2-LED-02 | M2 | outline | STUB | R5, R11 | SMT commitment v1 (`commitment_version = 1`) with non-membership proofs and typed `SlotView` proofs | `ledger::CommitV1` (`ledger/state_commitment`, R5); `read/views` `SlotView` v0 account leaves and tree levels, built O(state) per publish (R11) | SMT vectors; proof ≈ 768 B; timing; b3verify switched; no O(state) work per publish | M2/M2-LED-02/ | plan §3.7, O-9 |
| M2-LED-03 | M2 | outline | STUB | R5 | Merkle-sum liability tree per asset | `ledger::LiabilitySumRoot` (`ledger/state_commitment`) | sums in [0, 2^62]; inflation detected | M2/M2-LED-03/ | PLAN-3 |
| M2-LED-04 | M2 | outline | STUB | R5 | `exit_value` per leaf | `ledger::ExitValue` (`ledger/state_commitment`) | vectors | M2/M2-LED-04/ | PLAN-16 |
| M2-LED-05 | M2 | outline | STUB | R5, R7, R12 | WITHDRAW → pending reservation with a deterministic capacity bound | `LedgerState::RequestWithdrawal` (`ledger/ledger`, R5); engine branch `PhaseOf` WITHDRAW (`engine/engine`, R7; `ValidateSlotInputs` refuses the kind first); admission `KindAdmissible` WITHDRAW → `UNSUPPORTED_KIND` with reason M2-LED-05 (`admission/admission`, R12); the walking skeleton's check N9 expects exactly that refusal (`sim/scenario.cpp`, R14), so this task updates N9 and ARCHITECTURE.md 14.5 | withdrawal and two-market conflict tests; conservation includes pending | M2/M2-LED-05/ | TRD-21 |
| M2-LED-06 | M2 | outline | STUB | R5, R7, R12 | `SPOT_TO_FUTURES` / `FUTURES_TO_SPOT` cash transfers with a risk gate | `LedgerState::TransferToFutures`, `TransferFromFutures` (`ledger/ledger`, R5); engine branches `PhaseOf` (`engine/engine`, R7); admission `KindAdmissible` → `UNSUPPORTED_KIND` with reason M2-LED-06 (`admission/admission`, R12) | model A-D7 cases; unknown risk refuses | M2/M2-LED-06/ | TRD-13 |
| M2-LED-07 | M2 | outline | REAL-REPLACE | R5 | Model-exact duplicate-fact rules; also decide whether a zero-amount or protocol-target deposit fact is a certified refusal instead of an invalid slot (O-34) | `LedgerState::CreditDeposit` (`ledger/ledger`; the outline refuses every second fact for an outpoint) | differential cases | M2/M2-LED-07/ | TRD-3 |
| M2-LED-08 | M2 | todo | NONE | — | Stateful ledger fuzz ≥ 24 CPU-h | new | 0 findings | M2/M2-LED-08/ | plan M2 |
| M2-LED-09 | M2 | todo | NONE | — | Cross-OS roots, including MSVC `int128` fallback vectors | new | identical roots | M2/M2-LED-09/ | plan M2 |
| M2-LED-10 | M2 | outline | REAL-REPLACE | R9 | Copy-on-write candidate state | `BuildCandidate` (`state/candidate`): the engine copies the whole parent state for every `StateOwner::Preview`, `Reexecute` and replayed slot | same roots; work O(changed) | M2/M2-LED-10/ | PERF-12 |
| M2-ADM-01 | M2 | outline | REAL-REPLACE | R4, R12 | Exhaustive nonce/expiry tests plus a Quint model of D7 | `core/nonce_rule` (`ClassifyNonce`, `NonceInsert`, `InExpiryWindow`, `ExpiredAt`; R4); `admission/admission` `WindowRefusal` (R12) | model equals code on bounded traces | M2/M2-ADM-01/ | PLAN-1, D7 |
| M2-ADM-02 | M2 | todo | NONE | — | Account-exists check before crypto, under load | extends R12 | 10^5 `NO_ACCOUNT` frames cause 0 signature verifications | M2/M2-ADM-02/ | PLAN-9 |
| M2-ADM-03 | M2 | todo | NONE | — | `EXPIRED` proof against the SMT | `VerifyExpired` | forged variants fail | M2/M2-ADM-03/ | PLAN-1, needs M2-LED-02 |

## M3 — [CC] C3 order lifecycle

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M3-ENG-01 | M3 | outline | REAL-REPLACE | R6 | 2^17-tick band array, two-level bitmap, Fenwick tree, FIFO pool, open-addressing `OrderId` table (never iterated) | `engine/book` `OrderBook` (`std::map` of FIFO `std::deque` levels plus a lookup `std::map`; no capacity, no `BOOK_FULL`; O-35) | differential against the skeleton on 10^5 ops; `BOOK_FULL` | M3/M3-ENG-01/ | plan §3.5, D6 |
| M3-ENG-02 | M3 | outline | STUB | R6, R7 | Residual same-slot max-volume uniform-price call | `ResolveResidualCross` (`engine/residual`, R6; `FindResidualCross` already detects the crossing remainders); the certified refusal `REJ_RESIDUAL_CROSS_UNSUPPORTED` in engine phase 6 and `RefusesRemainderOnly` (`engine/engine`, R7; O-40) | checker covers the tie-break | M3/M3-ENG-02/ | D4, O-5, O-38, BLOCKED-ON-RULING |
| M3-ENG-03 | M3 | outline | STUB | R3, R6 | ±50% price band against the epoch reference price | `PriceBandCheck` (`engine/matcher`, R6; takes a `ReferencePrice`); `core/params` `Validate` (`price_band_enabled`, R3) | band edge tests | M3/M3-ENG-03/ | D6, O-38, BLOCKED-ON-RULING (reference source) |
| M3-ENG-04 | M3 | outline | REAL-REPLACE | R6, R7 | Complete caps: `FILL_CAP`, fills per slot, 8,192 changed leaves, `SLOT_FULL`, `BOOK_FULL`; rule how the slot fill budget is shared across markets and sides and which certified code a slot-capped remainder gets (no `SLOT_FULL` outcome code exists) | `engine/matcher` `SweepA2` (per-taker `FILL_CAP` and the `SlotFillBudget`: markets ascending, BUY before SELL, R6); `engine/engine` `ValidateSlotInputs` caps, one `SlotFillBudget` per slot shared by every market, and `REJ_FILL_CAP` for the remainder of a taker stopped by either cap (R7; O-40) | cap−1 / cap / cap+1 tests | M3/M3-ENG-04/ | D6, O-36 |
| M3-ENG-05 | M3 | todo | NONE | — | Independent checker written from the spec; shares only codecs and hashes, never engine, fee or nonce code; it also recomputes the walking skeleton's goldens that are pinned from the C++ output (`GOLDEN_CANONICAL_ORDER`, the slot-5 header hash and the final root, O-87) | new | 100% agreement, including adversarial and dust batches | M3/M3-ENG-05/ | plan M3, O-3 |
| M3-ENG-06 | M3 | todo | NONE | — | x86 gate: apply + roots p99 ≤ 5 ms at 1,024 actions on an adversarial 131k-tick book; parallel subtree hashing | new | perf evidence with PERF-7 metadata | M3/M3-ENG-06/ | plan §3.5 |
| M3-ENG-07 | M3 | todo | NONE | — | MEV analysis: A versus A2 | new | report accepted by the owner | M3/M3-ENG-07/ | D4, OWNER-GATED |
| M3-ENG-08 | M3 | outline | STUB | R3, R4 | `PER_SIDE` fee incidence plus D6 dust vectors | `core/params` `Validate` (`PER_SIDE`, R3); `core/fee_math` extension point (R4) | both incidences pass | M3/M3-ENG-08/ | D3, D5, D6, BLOCKED-ON-RULING |
| M3-ENG-09 | M3 | outline | STUB | R7, R12 | `CANCEL_ALL` | engine branch `PhaseOf` CANCEL_ALL (`engine/engine`, R7; `ValidateSlotInputs` refuses the kind first); admission `KindAdmissible` → `UNSUPPORTED_KIND` with reason M3-ENG-09 (`admission/admission`, R12) | releases all; conservation | M3/M3-ENG-09/ | plan §3.6 |
| M3-ENG-10 | M3 | todo | NONE | — | Replace/amend revisions | new | atomic replace tests | M3/M3-ENG-10/ | BLOCKED-ON-RULING |
| M3-ENG-11 | M3 | outline | REAL-REPLACE | R3 | Per-market tick, lot and minimum size at listing | `core/market` `MarketDescriptor` | vectors | M3/M3-ENG-11/ | D6, OD-7 |
| M3-ENG-12 | M3 | outline | REAL-REPLACE | R7 | Seed hardening: parent-header seed, grinding tests; consider a VRF or a unique threshold signature | `OrderKey`, `CanonicalOrder` (`engine/engine`) | no measured proposer or client advantage | M3/M3-ENG-12/ | PLAN-7, D4, O-4, O-4b, BLOCKED-ON-RULING |
| M3-ENG-13 | M3 | outline | REAL-REPLACE | R6 | Self-trade prevention policy | `SweepA2` self-match path (`engine/matcher`: a maker and a taker of one account trade like any other pair) | policy tests | M3/M3-ENG-13/ | O-17, BLOCKED-ON-RULING |
| M3-ENG-14 | M3 | outline | REAL-REPLACE | R6 | Ratify taker-to-maker pairing and the remainder rule | `AllocateProRata` and the `SweepA2` pairing (`engine/matcher`) | checker agreement | M3/M3-ENG-14/ | D4, O-5, O-37, BLOCKED-ON-RULING |

## MB1 and B0 — b3d regtest consensus at P (OWNER-GATED: outside `src/flowmesh2/`)

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| MB1-B3D-01 | MB1 | todo | NONE | — | `VaultV2` outputs | b3d | CONS-13 checklist | MB1/MB1-B3D-01/ | plan §3.6, D2, OWNER-GATED |
| MB1-B3D-02 | MB1 | outline | STUB | R4 | `CheckpointV2` MPA record and validation, including conservation | `core/facts` `CheckpointAcceptedFact` codec and `FactId` (R4); b3d | a forged root over the vault is refused | MB1/MB1-B3D-02/ | PLAN-3, OWNER-GATED |
| MB1-B3D-03 | MB1 | todo | NONE | — | Caps, 360-block delay, `PauseV2` | b3d | boundary tests | MB1/MB1-B3D-03/ | PLAN-4, PLAN-6, OWNER-GATED |
| MB1-B3D-04 | MB1 | todo | NONE | — | `Q_min`, maturity, pacing | b3d | boundary tests | MB1/MB1-B3D-04/ | PLAN-5, D16, OWNER-GATED |
| MB1-B3D-05 | MB1 | todo | NONE | — | V1 root pin | b3d | V1 checkpoints after P rejected | MB1/MB1-B3D-05/ | PLAN-15, OWNER-GATED |
| MB1-ADP-06 | MB1 | outline | STUB | R10 | Trusted-facts adapter that refuses MAIN/TESTNET/TESTNET4/SIGNET at compile time and start-up; it must deliver only batches that pass `CheckBatch` (O-34, O-58) | `chain/b3d_adapter` `B3dTrustedFactsAdapter::Create` (the start-up refusal is real; REGTEST is the stub; the compile-time refusal comes with the node link) | refusal tests; facts equal a block scan | MB1/MB1-ADP-06/ | T23, D8 |
| MB1-ADP-07 | MB1 | todo | NONE | — | Synchronous authority epoch in `ConnectTip`/`DisconnectTip` | b3d | a reorg at or below the anchor bumps the epoch; above it does not | MB1/MB1-ADP-07/ | PLAN-11, CONS-10, OWNER-GATED |
| MB1-REG-08 | MB1 | todo | NONE | — | Append-only MPA/wire registry | new | duplicate-number lint | MB1/MB1-REG-08/ | C-20, D14 |
| MB1-TST-09 | MB1 | todo | NONE | — | CONS-13 functional tests; fuzz ≥ 24 CPU-h | new | reports | MB1/MB1-TST-09/ | CONS-13 |
| B0-B3D-01 | B0 | todo | NONE | — | Bounded per-block undo of derived state, equal to full replay | b3d | undo equals replay; the 97 s replay is gone | B0/B0-B3D-01/ | CONS-3, OWNER-GATED |

## M4 — [CC] C4 agreement and storage

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M4-AGR-01 | M4 | outline | STUB | R10 | PBFT safety rules mapped from `test/flowmesh_v2_agreement/PROTOCOL.md` | `PbftSafetyRules` `MayPrepare`, `MayCommit`, `OnViewChange` (`agreement/pbft`) | trace agreement with the reference; Twins; the rules state the cross-epoch rule: a seat never signs a COMMIT for a height its own commit log decided with another header, whatever the epoch or view (a new epoch is a new `VoteKey`, so the signer log alone allows it), and a DECISION durable but refused at check 3 stays final for its height (O-89; the dev certifier's `DEV_HEIGHT_DECIDED` is the outline's form) | M4/M4-AGR-01/ | D10 |
| M4-AGR-02 | M4 | outline | STUB | R10 | `PbftAgreement`: execute-then-vote, `Reexecute`, BLS aggregation, parent certificate in proposals; replaces the dev certifier's `OnMessage`; rules the message shapes the outline chose (O-57); adds typed durable send paths for VIEW_CHANGE and NEW_VIEW, which `SeatMesh::Send` passes without a token today (O-79); builds certificates only from durable votes (`OutboundVote`): `SeatSigner::Sign` returns a raw `VoteSignature` that could today enter a `SlotCertificate` or an opaque BULK frame; runs M4-AGR-12's fact check before every vote | `PbftAgreement::Create` (`agreement/pbft`); `DevSingleSeatCertifier::OnMessage` (`dev/dev_certifier`, `Unsupported`) | N=4 and N=7 DST with 0 violations | M4/M4-AGR-02/ | plan §3.4.1, D10 |
| M4-AGR-03 | M4 | outline | STUB | R10 | 8-slot terms, seed-chosen leader, slow-set skip | `LeaderSchedule::LeaderFor` (`agreement/leader`) | distribution and skip tests | M4/M4-AGR-03/ | D11, BLOCKED-ON-RULING |
| M4-AGR-04 | M4 | outline | STUB | R10 | Inclusion lists; decide whether the vote signature covers the inclusion IDs (O-57) | `InclusionList::Build`/`Check` (`agreement/inclusion`) | a withheld action is forced into s+1 | M4/M4-AGR-04/ | PLAN-7 |
| M4-AGR-05 | M4 | todo | NONE | — | Adaptive timeouts | new | G-FAULT profile | M4/M4-AGR-05/ | plan §3.4.1 |
| M4-AGR-06 | M4 | outline | STUB | R10 | Restart high-water statements from 2f+1 seats | `RestartFence::Collect` (`agreement/restart_fence`) | a rolled-back VM retires the key | M4/M4-AGR-06/ | PLAN-8, DUR-3 |
| M4-AGR-07 | M4 | outline | STUB | R10 | Conflicting-certificate evidence and halt; rule whether two COMMIT votes of one seat for one height under different epochs are evidence (`CheckEvidence` requires equal epochs today, so the cross-epoch double vote of O-89 would not be recorded) | `EvidenceStore::Record`/`Load` (`agreement/evidence`; the `Evidence` codec and `CheckEvidence` are real, `agreement/messages`) | evidence persisted; halt | M4/M4-AGR-07/ | SEC-11 |
| M4-AGR-08 | M4 | todo | NONE | — | Invalid-share exclusion | new | forged-vote profile ≤ 2× idle | M4/M4-AGR-08/ | plan §3.4.1 |
| M4-AGR-09 | M4 | todo | NONE | — | Apalache N=4 to depth 12; N=7 random ≥ 10^6 traces; Twins | new | results | M4/M4-AGR-09/ | plan M4, D9 |
| M4-AGR-10 | M4 | outline | STUB | R9 | Pipelined `Preview` over an uncommitted parent candidate | `StateOwner::Preview` with a `ParentRef` of kind `CANDIDATE` (`state/state_owner`) | equals sequential roots | M4/M4-AGR-10/ | DEFERRED by plan §13 (measured need) |
| M4-AGR-11 | M4 | outline | REAL-REPLACE | R7 | Validity predicate conformance with `PROTOCOL.md`; ratify the clauses the outline added (O-41) | `engine/engine` `ValidateSlotInputs` | checker and Twins agreement | M4/M4-AGR-11/ | plan §3.4.1 |
| M4-AGR-12 | M4 | todo | NONE | — | Follower fact completeness: before it votes, a follower compares the proposal body's facts with the facts its own chain adapter delivers for [facts_from, facts_to) at the proposal's anchor, and refuses a body that drops, adds, reorders or alters a fact. `ValidateSlotInputs` and `ReexecuteOn` check only that the body is consistent with the header, so a faulty leader can drop a deposit and move the fact cursor past it for good | new, in the agreement's vote path (`PbftAgreement`, M4-AGR-02); the adapter seam `ChainAdapter::Poll` | failing first: a leader that drops a deposit and advances `facts_to` past it gets an honest follower's vote; then refused, and the deposit is credited by a later slot | M4/M4-AGR-12/ | review should-fix 5, O-34, O-58 |
| M4-STO-01 | M4 | outline | REAL-REPLACE | R8 | `FileJournal` recovery: torn tail; a gap or CRC break retires the key; `DECISION`-body replay; rebuild the signer log's equivocation index and the commit log's decision index from a journal with history; decide whether a segment needs a header naming its role and instance (O-46), and how instance ids stay unique per role (`Confirm` checks the role since O-91, but two journals of one role with one instance id are indistinguishable) | `FileJournal::OpenExisting`/`ReadAll` (`storage/file_journal`); `SignerLog::Create` and `CommitLog::Create` over a journal that already has records (`storage/signer_log`, `storage/commit_log`) | crash-at-every-barrier tests | M4/M4-STO-01/ | DUR-1, DUR-2 |
| M4-STO-02 | M4 | outline | STUB | R8 | Group-commit `LogWriter` thread over `Pending`/`Confirm` | `LogWriter::Start` (`storage/log_writer`) | one barrier per group; TSan clean | M4/M4-STO-02/ | plan §3.7 |
| M4-STO-03 | M4 | outline | STUB | R8, R9 | LevelDB snapshots plus exactly-once replay; the `SNAPSHOT_MARK` record writer | `SnapshotStore::Write`/`Load` (`storage/snapshot_store`, R8); `StateOwner::Restore` and `ReplayDecisions` on a `SNAPSHOT_MARK` record (`state/state_owner`, `state/replay`, R9; replay starts at genesis until then); `Restore` must re-check the restored state's invariants (O-54); at start-up the action pool (R12) is empty, so the node must also rebuild the pool's retained `INCLUDED` records of the last `retention_slots` slots from the commit journal, or an exact resend of an included action is answered by A5/A6 instead of `DUPLICATE` (O-71); `Restore` installs `ReplayResult::head` (the last APPLIED slot) and must run check 3 under the current authority on every decided slot without APPLIED (a crash before B-APPLIED, or a DECISION durable but refused at check 3) before it writes APPLIED; a refusal keeps the head there and the node fenced, and that height is never re-decided with another header (O-89) | restart byte-equal to an uninterrupted run; a restart after a check-3 refusal gives the live head and seals nothing at that height | M4/M4-STO-03/ | DUR-1 |
| M4-STO-04 | M4 | todo | NONE | — | kill -9 + LazyFS campaign | new | 0 equivocations; exactly-once | M4/M4-STO-04/ | D9, OWNER-GATED |
| M4-STO-05 | M4 | outline | STUB | R3, R8 | Folded signer mode (sign, then one barrier) and A/B measurement | `SignerLog::AppendSignedFolded` (R8); `core/params` `Validate` (`FOLDED_SIGNING`) | A/B evidence; strict stays the default | M4/M4-STO-05/ | D15, BLOCKED-ON-RULING |
| M4-STO-06 | M4 | outline | STUB | R8 | Segment rollover | `FileJournal::Rollover` and any append that no longer fits in the segment (`FileJournal::AdmitAppend`; refused without fencing) (`storage/file_journal`) | crash tests | M4/M4-STO-06/ | plan §3.7 (S-9) |
| M4-STO-07 | M4 | outline | STUB | R8, R9 | Durable PBFT view state: `VIEW_STATE`, `NEW_VIEW_ACCEPTED`, `PREPARED_QC` | `SignerLog::AppendViewState`, `SignerLog::AppendNewViewAccepted` (`storage/signer_log`); `CommitLog::AppendPreparedQc` (`storage/commit_log`); the record structs are target shapes (R8); `ReplayDecisions` on a `PREPARED_QC` record (`state/replay`, R9) | restart-in-view-change tests | M4/M4-STO-07/ | PROTOCOL.md, O-16 |
| M4-NET-01 | M4 | outline | REAL-REPLACE | R13 | Implemented regtest TCP seat mesh: 4 independently bounded lanes, BIP324 encryption and mutual BIP340 session authentication | `TcpSeatMesh::Create` (`net/tcp_mesh`; target shapes `TcpMeshConfig`, `SeatEndpoint`, `DEFAULT_SEAT_MESH_PORT` 5649); a transport implements only `Transmit` and `Poll` of `net/seat_mesh` `SeatMesh`, whose send rules are real (O-76, O-79) | crypto review; interop | M4/M4-NET-01/ | D12, OWNER-GATED |
| M4-NET-02 | M4 | outline | STUB | R13 | SipHash 6-byte short IDs and body fetch | `ShortIdCodec::Encode`/`Decode` (`net/short_id`; keyed by header hash and nonce) | collision and fetch tests | M4/M4-NET-02/ | PLAN-10 |
| M4-NET-03 | M4 | outline | STUB | R13 | Sentry fan-out | `SentryFanout::Fanout` (`net/sentry`; target shape `FanoutReport`) | fan-out tests | M4/M4-NET-03/ | PLAN-10 |
| M4-NET-04 | M4 | todo | NONE | — | Protocol-derived budgets; also replaces the provisional lane limits and decides deficit round-robin below CRITICAL (O-77); bounds the reserved classes: a frame that merely decodes as a CANCEL gets the reserved class without a signature check (`InProcessApi::ClassOf`), and reserved classes may fill the whole lane, so unsigned fake cancels can fill the ACTION lane before admission sees them | new; replaces `net/lanes` `ProvisionalLaneLimits`; `net/in_process_api` `ClassOf` | budget tests; a flood of unsigned cancels keeps neither ordinary actions nor real cancels out of the lane | M4/M4-NET-04/ | D13 |
| M4-ADM-01 | M4 | outline | STUB | R4, R12 | `CCheckQueue` batch BIP340 plus `CuckooCache` | `core/action` `BatchVerifier::VerifyBatch` (R4); admission batch path `Admission::AdmitBatch` (`admission/admission`, R12) | same results as sequential | M4/M4-ADM-01/ | plan §3.2 |
| M4-ADM-02 | M4 | outline | STUB | R12 | Credit budgets; rule whether a budget charges per attempt or per admission (O-73) and how the lanes' reserved CANCEL share is divided between accounts (O-77) | `admission/budget` `CreditBudget::Create` and `Check` (`CreditBudgetParams` holds the plan's proposed defaults) | budget and cancel-lane tests | M4/M4-ADM-02/ | SEC-4, D13, BLOCKED-ON-RULING |
| M4-ADM-03 | M4 | todo | NONE | — | One action must not stall sealing: when the slot's `Preview` fails with an engine error that one action causes (the O-34 errors, for example a 257th asset in one account or cumulative deposits of an asset above 2^62 atoms, only about 4.6 units of an 18-decimal asset), exclude that action so it reaches a terminal pool status and a definite client answer, instead of staying IN_PROPOSAL and being taken again every slot; or turn those errors into certified refusals (M2-LED-07). Today the node returns the error and retries the slot without fencing (O-90) | `sim/sim_node` `SealSlot` step 3; `admission/pool` `TakeForSlot` (IN_PROPOSAL entries are taken again); `engine/engine` | failing first: a slot carrying one such action never seals; then it seals without it, the action is terminal, and no other action waits more than one slot | M4/M4-ADM-03/ | O-34, O-90, review should-fix 5 |
| M4-SIM-01 | M4 | outline | STUB | R13, R14 | Multi-node DST (N=4, 7) ≥ 1,000 CPU-h over an in-process mesh | `InProcessSeatMesh::Create` (`net/in_process_mesh`, R13); `SimCluster::Create(n>1)` (`sim/cluster`, R14; a cluster of one node is real) | 0 violations | M4/M4-SIM-01/ | plan M4 |
| M4-SIM-02 | M4 | outline | STUB | R14 | Crash, drop, delay, partition and skew hooks | `CheckFaultPlan` (`sim/faults.h`, R14): the target shapes `crash_before_slot`, `DropRule`, `DelayRule`, `partition`, `SkewRule`; `SimNode::Create` runs it first. The three real faults (withhold, the commit-barrier failure, the authority bump) stay | each hook exercised | M4/M4-SIM-02/ | plan L4 |
| M4-SIM-03 | M4 | outline | STUB | R14 | Twins and mutation fuzzing | `SimCluster::CreateTwins` (`sim/cluster`, R14) | the Twins set passes | M4/M4-SIM-03/ | plan M4 |
| M4-SEP-01 | M4 | todo | NONE | — | Separate-process fault campaign plus an independent issued-signature checker | new | [CC] C4 exit | M4/M4-SEP-01/ | [CC] C4 |

## M5 — [CC] C6 API, b3verify, Hive

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M5-API-01 | M5 | outline | STUB | R13 | HTTPS + SSE (TLS 1.3, 0-RTT off) | `HttpsSseApi::Create` (`net/https_sse_api`; target shape `HttpsSseConfig`; a `ClientApi` in front of a backend) | plan §3.8 endpoint tests | M5/M5-API-01/ | SEC-7, OWNER-GATED |
| M5-API-02 | M5 | outline | STUB | R13 | Streams: epoch/seq/head envelope, snapshot plus deltas, heartbeat, resubscribe | `InProcessApi::Subscribe` (`net/in_process_api`; target shapes `StreamChannel`, `StreamCursor`, `StreamRequest`, `StreamId` in `net/client_api`) | gap tests | M5/M5-API-02/ | UX-10 |
| M5-API-03 | M5 | todo | NONE | — | Held-lock nemesis (10 s) | new | reads stay within budget | M5/M5-API-03/ | PERF-12 |
| M5-RD-01 | M5 | outline | REAL-REPLACE | R11 | RCU archive and typed view index; outcome-index retention ≥ `max_delay`; decide whether a handle pins its view and whether `Publish` binds the token to a commit-journal instance (O-64, O-65) | `read/read_service` archive: `ReadService` view map and outcome index under the archive mutex, one lookup per reply | no publisher mutex on reads; retention invariant | M5/M5-RD-01/ | PERF-12, DUR-5 |
| M5-VFY-01 | M5 | todo | NONE | — | b3verify API v1 | `verify/` | API document and tests | M5/M5-VFY-01/ | SEC-5 |
| M5-VFY-02 | M5 | todo | NONE | — | Checkpoint leaf checks and alerts | new | alert tests | M5/M5-VFY-02/ | plan §3.12 |
| M5-VFY-03 | M5 | outline | STUB | R11 | Liability path bounds | `verify/b3verify` `VerifyLiabilityPath` (target shape `LiabilityPath`) | a violation is detected | M5/M5-VFY-03/ | PLAN-3 |
| M5-CLI-01 | M5 | todo | NONE | — | SQLite WAL outbox: exact bytes, hedged dual send, prewarm | new | crash/resend tests; never re-signs | M5/M5-CLI-01/ | DUR-4 |
| M5-QT-01 | M5 | todo | NONE | — | Engine-off Hive trade page | Qt | R3 p95 ≤ 30 ms; first book p95 ≤ 50 ms | M5/M5-QT-01/ | UX-2, UX-4, UX-5, UX-7, OWNER-GATED |

## M6 — DEX-1 = [CC] C7

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M6-DEX-01 | M6 | todo | NONE | — | 4-operator regtest: real processes, fsync, b3d adapter, rUSD, both markets, checkpoint withdrawals, engine-off Qt | new | evidence bundle | M6/M6-DEX-01/ | [CC] C7, D8 |
| M6-DEX-02 | M6 | todo | NONE | — | 72 h soak; G-COLO, G-LOAD and G-BLOCK at n ≥ 1,000 | new | gate reports | M6/M6-DEX-02/ | PERF-3, PLAN-12 |
| M6-DEX-03 | M6 | todo | NONE | — | Packaging decision (one process or split) | new | report | M6/M6-DEX-03/ | plan §3.2 |
| M6-DEX-04 | M6 | todo | NONE | — | Link `fm2_core` into b3d | new | the `nm` guard still passes | M6/M6-DEX-04/ | OWNER-GATED |

## M7 — adapter, seats, tape, governance (plan addition)

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M7-ADP-01 | M7 | outline | STUB | R10 | Finalized + 30-deep adapter with anchor proofs | `B3dFinalizedAdapter::Create` (`chain/b3d_adapter`) | reorg and epoch tests | M7/M7-ADP-01/ | TRD-4, PLAN-11 |
| M7-ADP-02 | M7 | outline | STUB | R11 | Anchor and inclusion proofs for clients | `verify/b3verify` `VerifyAnchorProof` (target shape `AnchorProof`) | forged proofs fail | M7/M7-ADP-02/ | plan §3.3 |
| M7-ADP-03 | M7 | todo | NONE | — | Bind the FlowMesh instance into `DomainHash`: it binds `ModernChainDomain`, `fm_version` and the network magic but no instance or genesis, so it assumes one FlowMesh instance per network; a second instance or a re-genesis on the same network would accept the other's signed actions and votes | `core/hashes` `DomainHash`; D1 | failing first: an action signed for one instance is admitted by another instance on the same network; then WRONG_DOMAIN | M7/M7-ADP-03/ | D1, review nit, OWNER-GATED |
| M7-SEAT-01 | M7 | outline | STUB | R4 | Seat registration facts (PoP, maturity, endpoints) | `core/facts` `SeatChangeFact` codec and `FactId` (R4); engine (`ValidateSlotInputs` refuses a decoded non-deposit fact with `SLOT_FACT_KIND` until each fact task adds its branch, R7) | the seat set changes only through facts | M7/M7-SEAT-01/ | FN-9, FN-10 |
| M7-SEAT-02 | M7 | todo | NONE | — | State sync for joining seats | new | a joining seat syncs and votes | M7/M7-SEAT-02/ | plan M7 |
| M7-FEE-01 | M7 | outline | REAL-REPLACE | R5 | Per-seat accrual; claims by BLS key; rule whether a TRANSFER may credit a protocol account (O-34) | seat-pool protocol account: `SeatPoolAccount` (`core/state_root`), `LedgerState::Credit` (`ledger/ledger`) | claim tests | M7/M7-FEE-01/ | FN-12, D5, C-3, BLOCKED-ON-RULING |
| M7-TAPE-01 | M7 | todo | NONE | — | Trade tape and candles with time provenance | new | tests | M7/M7-TAPE-01/ | UX-5 |
| M7-GOV-01 | M7 | outline | STUB | R4, R13 | Listing (100,000 B3) | `core/facts` `ListingFact` codec and `FactId` (R4); `Governance::ApplyListing` (`governance/governance`, R13) | listing tests; a 257th listed asset is refused while `MAX_LEAF_ASSETS` is 256 (O-34: a 257th asset in one account is an error, not an outcome), unless M2-LED-07 first makes it a certified refusal | M7/M7-GOV-01/ | AST-6, AST-8, OWNER-GATED |
| M7-GOV-02 | M7 | outline | STUB | R13 | Removal: cancel-only, then claim-only | `Governance::ApplyRemoval` (`governance/governance`; target shape `RemovalRequest`) | no-seizure tests | M7/M7-GOV-02/ | AST-6 |
| M7-GOV-03 | M7 | outline | STUB | R13 | Versioned parameter heights (`profile_hash` in the header) | `Governance::ParamsAt` (`governance/governance`) | boundary tests | M7/M7-GOV-03/ | GOV-2 |
| M7-PERF-01 | M7 | todo | NONE | — | k = 64 tests; G-WAN run | new | reports | M7/M7-PERF-01/ | plan M7, D-G1 |

## M8 — [CC] C8a: bridge, checkpoints, exit, cutover

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M8-BRG-01 | M8 | outline | STUB | R13 | bUSD mint → `VaultV2` deposit | `BridgeManagerV2::OnFinalizedMint` (`bridge/bridge_manager`; target shape `FinalizedMint`; the result is the `DepositFact` the chain adapter delivers) | anvil + regtest end to end | M8/M8-BRG-01/ | BRG-1, BRG-4, CLAUDE.md rules 5-6, C-6 |
| M8-BRG-02 | M8 | outline | STUB | R4, R13 | Withdrawal packets to `message_root`; payout facts | `core/facts` `PayoutConfirmedFact` codec and `FactId` (R4); `BridgeManagerV2::BuildWithdrawalPacket` (`bridge/bridge_manager`, R13; target shapes `WithdrawalRequest`, `WithdrawalPacket`) | no duplicate payout | M8/M8-BRG-02/ | BRG-5 |
| M8-BRG-03 | M8 | outline | STUB | R13 | Validity floor, caps, safe exporter | `BridgeManagerV2::CheckReleaseLimits` (`bridge/bridge_manager`; target shapes `ReleaseLimits`, `ReleaseWindow`) | cap tests | M8/M8-BRG-03/ | PLAN-14, D23, OWNER-GATED |
| M8-BRG-04 | M8 | todo | NONE | — | Sepolia/Hoodi deployment plus Foundry invariants | new | invariant runs | M8/M8-BRG-04/ | plan M8 |
| M8-CKP-01 | M8 | outline | STUB | R11, R13 | Checkpoint builder and verification; move the one checkpoint value type into `core/` (O-80) | `CheckpointBuilder::Build` (`bridge/checkpoint`, R13; target shape `Checkpoint`, the fields of `CheckpointClaim`); `verify/b3verify` `VerifyCheckpoint` (R11, target shape `CheckpointClaim`) | b3d accepts a valid and refuses a forged checkpoint | M8/M8-CKP-01/ | PLAN-3 |
| M8-EXIT-01 | M8 | outline | STUB | R4, R13 | Forced exit after 10,080 blocks; nullifiers | `core/facts` `ExitNullifierFact` codec and `FactId` (R4); `ForcedExit::Open`/`Redeem` (`bridge/forced_exit`, R13; `FORCED_EXIT_DELAY_BLOCKS` PROVISIONAL; target shapes `ExitClaim`, `ExitRedemption`) | drill | M8/M8-EXIT-01/ | PLAN-16, D20, OWNER-GATED |
| M8-MIG-01 | M8 | todo | NONE | — | V1 cutover: claims plus a transfer consuming the V1 nullifier | new | no double spend | M8/M8-MIG-01/ | PLAN-15, D25 |
| M8-MIG-02 | M8 | todo | NONE | — | Fork-id, sunset release, liability manifest | new | manifest derived twice, equal | M8/M8-MIG-02/ | D24 |

## M9 — [CC] C8b futures

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M9-FUT-01 | M9 | outline | STUB | R3, R13 | Dated isolated linear futures in Q; profile `futures_enabled` | `FuturesDomain::PlaceFuture` (`futures/futures`, R13; target shape `FutureOrder`); `core/params` `Validate` (`futures_enabled`) | contract tests | M9/M9-FUT-01/ | D21, BLOCKED-ON-RULING |
| M9-FUT-02 | M9 | outline | STUB | R13 | Oracle median over ≥ 2f+1 vote-committed submissions | `FuturesDomain::SubmitOracle` (`futures/futures`; target shape `OracleSubmission`) | manipulation simulations | M9/M9-FUT-02/ | PLAN-13 |
| M9-FUT-03 | M9 | outline | STUB | R13 | Margin and mark | `FuturesDomain::MarginCheck` (`futures/futures`; target shape `MarginState`) | vectors | M9/M9-FUT-03/ | plan §3.11 |
| M9-FUT-04 | M9 | outline | STUB | R7, R13 | Liquidation index, cap, insurance, deficit halt; engine phase 7 | `FuturesDomain::Sweep` (`futures/futures`, R13; target shape `SweepReport`); engine phase 7 `RunFuturesPhase` (`engine/engine`, R7; unreachable while `Validate` refuses `futures_enabled`) | storms stay within the slot budget | M9/M9-FUT-04/ | plan §3.11 |
| M9-FUT-05 | M9 | todo | NONE | — | Open-interest caps; economic simulations | new | reports | M9/M9-FUT-05/ | plan M9 |

## M10 — [CC] C5 remainder (b3d; OWNER-GATED, C-12)

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M10-B3D-01 | M10 | todo | NONE | — | Finality V2 | b3d | sign-versus-reorg proof first | M10/M10-B3D-01/ | CONS-4, CONS-8, C-12, OWNER-GATED |
| M10-B3D-02 | M10 | todo | NONE | — | PoS V2 fixes | b3d | shadow fork; U == U′ | M10/M10-B3D-02/ | CONS-3, D26, OWNER-GATED |
| M10-SPEC-01 | M10 | todo | NONE | — | Sign-versus-reorg linearization proof, replacing the outline's double authority check; it must cover an authority change after check 2 (during B-VOTE or between B-DECISION and `Apply`), which leaves a durable DECISION that check 3 refuses, and rule how such a decided-but-unapplied height is resolved across seats (applied once the authority returns, voided, or re-anchored), since the outline only halts and never signs another header for it (O-89) | spec | proof, including the check-3 case | M10/M10-SPEC-01/ | [CC] boundary 6, C5, CONS-10 |

## M11 — [CC] C9 release

| ID | Ms | Status | Skeleton | Slice | Task | Replaces / location | Acceptance (failing-first) | Evidence | Refs |
|---|---|---|---|---|---|---|---|---|---|
| M11-REL-01 | M11 | todo | NONE | — | External audits | — | high and critical findings fixed and re-reviewed | M11/M11-REL-01/ | SEC-9 |
| M11-REL-02 | M11 | todo | NONE | — | Bug bounty | — | programme report | M11/M11-REL-02/ | SEC-9 |
| M11-REL-03 | M11 | todo | NONE | — | Guix × 2 | — | matching hashes | M11/M11-REL-03/ | SEC-6 |
| M11-REL-04 | M11 | todo | NONE | — | Shadow fork plus ≥ 4 weeks of observer mode | — | observer data | M11/M11-REL-04/ | MIG-6 |
| M11-REL-05 | M11 | todo | NONE | — | Operator rehearsal | — | report | M11/M11-REL-05/ | [CC] C9 |
| M11-REL-06 | M11 | todo | NONE | — | Owner decides P and P_f | — | ruling | M11/M11-REL-06/ | CONS-12, OWNER-GATED |

---

## Requirement IDs not listed in plan §2.1

These come from the requirements baseline, which is not tracked in this
repository. One-line summaries:

| ID | Summary |
|---|---|
| DUR-5 | A duplicate returns the original outcome; an executed action consumes its nonce even on deterministic rejection. |
| DUR-6 | Definite outcomes (resting, filled, cancelled or a named reject); terminal pool-drop events; no silent drops. |
| TRD-21 | Withdrawal lifecycle reserved → certified → settlement pending → payout broadcast → confirmed; admission bounded by deterministic payout capacity. |
| SEC-4 | Admission windows, per-account cap, pool TTL, cancel lane, separate trade and read budgets; economic budgets need a ruling. |
| SEC-11 | Equivocation and certificate-conflict evidence is persisted and exposed. |
| UX-10 | Push updates: snapshot then deltas, watchdog, resubscribe. |
| FN-11 | FN duties: propagation, proposal, certification, state-root agreement, data availability, withdrawal-receipt certification. |
| CONS-3 | Lessons not to repeat: self-vote deadlock at 811,631; full finality replay under `cs_main` (97 s); stuck producer set; headcount Sybil. |
| CONS-13 | Every consensus change carries activation-boundary, connect/disconnect, restart, reindex, replay, two-node and adversarial tests. |
| C-3 | Who gets the 80% fee share: equal per seat ruled for v1, OPEN for V2. |
| C-12 | Modern PoS redesign needs an explicit owner ruling covering the greenfield scope. |
| C-20 | MPA type number collision; assign numbers in one append-only registry. |
| OD-7 | Per-market lot and tick precision (open decision; plan D6). |

## Mapping to [CC]

M1 = C1, M2 = C2, M3 = C3, M4 = C4, M5 = C6, M6 = C7, M8 + M9 = C8,
M10 (with B0 and parts of MB1) = C5, M11 = C9. M0 and M7 are plan additions.
The R slices map to no C-milestone exit.

Suggested Codex order (plan critical path M0 → M2 → M3 → M4 → DEX-1 → M8/M9 →
M11): M0-AUD-01 first; then M0 and M1 guards; then M2, M3 and M4 in dependency
order. MB1, B0 and M10 wait for owner authorization.

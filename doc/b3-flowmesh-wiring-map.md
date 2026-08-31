# FlowMesh → Node/Consensus Wiring Map

**Status: OWNER-SANCTIONED SPIKE, REGTEST-ONLY. Nothing here ships in the transition
release.** This document is the inventory of every link between the FlowMesh engine
(`src/flowmesh/`, `src/node/flowmesh_store.{h,cpp}`) and a running node, discovered by
reading the code on `experiment/flowmesh-consensus-wiring` (base commit `27a6456`).
Each link is classified **EXISTS / PARTIAL / MISSING** with file:line references.

The one-sentence verdict up front: **the engine, its durable store, and its
chain-anchor policy are finished and production-hardened, but not a single line of
node startup, networking, RPC, block-commitment, or seat-derivation code touches
them** — the only production entry point, `node::StartValidator`, has zero callers
outside `src/test/`. FlowMesh today is a complete machine with no ignition wiring.

---

## 1. What exists and how it works

### 1.1 The engine: `flowmesh::MeshNode` — EXISTS (complete, transport-agnostic)

`src/flowmesh/sync.h:236-692`. One object per validator identity, deliberately
NOT thread-safe (`sync.h:72-74`) and NOT copyable/movable (`sync.h:278-282`); the
eventual network wiring owns serialization. The full pipeline is implemented:

- `TryPropose` (`sync.h:375`) — proposer-schedule check, anchor policy,
  microblock construction from the pool.
- `HandleProposal` (`sync.h:427`) — mandated guard order (shape → sequence →
  round → lock → proposer → anchors → evidence → execute/cache), durable
  compare-and-set lock **before** attesting.
- `HandleAttestation` (`sync.h:489`) — threshold certificate assembly,
  equivocation evidence capture.
- `HandleCertified` (`sync.h:529`) — certified-entry adoption, certificate-conflict
  fail-stop (owner ruling 2026-08-22).
- Catch-up: `MaybeRequestCatchup`/`HandleCatchupRequest`/`HandleCatchupResponse`
  (`sync.h:580-619`).
- Fail-stop halts: `MeshHalt` (`sync.h:227-234`) — persist failure, lock-journal
  failure, anchor invalidation, certificate conflict.

Signing construction is sealed: the full constructor is private
(`sync.h:302-323`), reachable only through `detail::SigningNodeFactory`
(`sync.h:715-738`), whose only friends are `node::StartValidator` and the
test-only bridge. Public construction is observer-only (`sync.h:284-295`).

### 1.2 The durable store: `node::FlowMeshStore` — EXISTS (complete)

`src/node/flowmesh_store.h:45-169`, `flowmesh_store.cpp`. LevelDB-backed
(`CDBWrapper`) certified log with atomic append+marker (`flowmesh_store.cpp:225-256`),
compare-and-set lock journal (`:265-323`), fully verified replay
(`:357-438`), and certificate-verified snapshots with full-prefix anchor
revalidation (`:483-601`). One signing validator per store enforced atomically
(`flowmesh_store.h:73-74`). Format v2; v1 markers fail closed
(`flowmesh_store.h:50-52`).

### 1.3 The production startup lifecycle: `node::StartValidator` — EXISTS, **ZERO CALLERS**

`src/node/flowmesh_store.cpp:603-687` (declared `src/flowmesh/sync.h:702-705`).
It does everything a node would need at start:

1. Validates config (auth/anchors/schedule non-null, curve bound, base ≠ quote).
2. Builds the **canonical empty genesis state** internally from the immutable
   market configuration (`vault_commitment`, `base_asset`, `quote_asset`, `max_k`)
   — callers cannot inject fabricated balances (`flowmesh_store.cpp:620-626`).
3. Store-neutral validation: `CheckForDomain` → snapshot-accelerated verified
   replay → lock restore, all **before** the only store mutation
   (`OpenForDomain`) (`flowmesh_store.cpp:639-669`).
4. Wires `StoreCommitSink`/`StoreLockJournal`/`StoreCatchupSource` and constructs
   the signing `MeshNode` via the factory (`flowmesh_store.cpp:670-686`).

`grep -rn "StartValidator" src/` outside `src/flowmesh/sync.h` and
`src/node/flowmesh_store.*` hits **only `src/test/flowmesh_sync_tests.cpp` and
`src/test/util/flowmesh.h`**. Nothing in `src/init.cpp`, `src/node/`, or any
production translation unit constructs a `FlowMeshStore` or calls
`StartValidator`. This is the central missing link.

### 1.4 Anchor policy — EXISTS on both sides, never connected at startup

- Interface: `flowmesh::AnchorPolicy` (`src/flowmesh/deposit.h:77-93`) —
  `Acceptable` (canonical + buried), `StillCanonical` (depth-free), `Current()`.
- Production implementation: `node::ChainAnchorPolicy`
  (`src/node/flowmesh_anchor.h:41-53`, `flowmesh_anchor.cpp:14-55`) reads the
  live `ChainstateManager` under `cs_main`. Compiled into the node binary
  (`src/CMakeLists.txt:249`) but **instantiated nowhere in production**.
- Depth: `FLOWMESH_ANCHOR_DEPTH = 30` is RATIFIED (owner ruling 2026-08-22,
  `flowmesh_anchor.h:29-39`).
- Anchors in a live chain therefore come from `ChainAnchorPolicy::Current()`
  (active tip minus 30); a fresh chain shorter than the depth yields the null
  anchor (`flowmesh_anchor.cpp:46-55`), which microblocks may carry
  (`deposit.h:32`, `StillCanonical` trivially passes null).
- MISSING: no code creates a `ChainAnchorPolicy` and hands it to a MeshNode;
  no hook re-runs `RecheckCommittedAnchors()` when the B3 tip changes (a
  `ValidationSignals`/`CValidationInterface` subscriber would be the natural
  wire — none exists for FlowMesh).

### 1.5 Deposits — PARTIAL by design (fail-closed)

- Interface `flowmesh::DepositVerifier` (`deposit.h:59-65`): answer from
  canonical B3 chain data at an anchor.
- The only production implementation is `node::UnavailableDepositVerifier`
  (`flowmesh_anchor.h:62-70`): **always refuses**. Real recognition requires
  DEX_VAULT outputs on-chain, which cannot exist yet (see §2.1). The
  account-binding rule for `VAULT_KIND_USER_DEPOSIT` params is declared
  (`src/modern/policy.h:121-136`) but no scanner maps chain outputs →
  `DepositInfo`. MISSING: a `ChainDepositVerifier` reading DEX_VAULT v2 outputs
  from the UTXO set/at-anchor view, gated on asset-policy activation.

### 1.6 Quorum / seats — PARTIAL (mechanics exist, derivation is an owner decision)

- Shape validation + threshold math: `certificate.h:133-148`
  (`MinCertificateThreshold`, fault bound `f` is an OWNER DECISION).
- Proposer schedule: `RoundRobinSchedule` (`recovery.h:70-88`) — explicitly a
  PROVISIONAL DEFAULT pending ratification.
- MISSING entirely: derivation of the seat set from chain state. The design says
  seats are FN validators (`certificate.h:163-166` "seats derive from anchored B3
  state (FN seat lifecycle: OWNER DECISION)"), FN policy outputs exist as data
  model (`modern/policy.h:102` `PolicyType::FN = 5`) but FN v1 is NOT activated
  (`policy.h:91-94`, `IsActivatedPolicy` returns false for FN). Seat ROTATION is
  deliberately unsupported in the store (`flowmesh_store.h:31-33`): a quorum
  change fails the log closed (`flowmesh_store.cpp:171-177`). **Wiring FlowMesh
  for real requires the FN seat lifecycle ruling first; nothing can be coded
  around it.**

---

## 2. What is missing, link by link

### 2.1 Committing FlowMesh state into B3 blocks — MISSING (no record type exists)

How other modern objects gate, for comparison:

- Era/activation predicate: `Consensus::ModernObjectRulesActive`
  (`src/consensus/era.h:159-163`) = legacy chain + H set + X pinned + modern-PoS
  rules present. Production networks ship without the pin, so everything modern
  is fail-closed.
- Modern Payload Area registry (`src/modern/mpa.h:33-41`, `GetPayloadTypeStatus`
  `:72-89`): frozen types 1-5 only — FN claim, legacy FN issuance, asset
  issuance (all INACTIVE), FINALITY_CERTIFICATE and FINALITY_KEY_EVIDENCE
  (ACTIVE only under `ModernObjectRulesActive`). Unknown → invalid.
- Metadata cells (`modern/policy.h:96-119`): policy types 6/7/8
  (FINALITY_CERT / FINALITY_KEY / MODERN_PAYLOAD_ROOT), coinbase carriers, MPA
  payload root committed via `ComputePayloadRoot` (`modern/payload_root.h:100`).
- DEX_VAULT policy outputs: declared at v2 (`policy.h:121-136`), activated only
  when `assets_active` (`IsActivatedPolicy`, `policy.h:178-197`), which is
  sourced from `Params::test_only_asset_policies_active`
  (`consensus/params.h:226`) — **false on every real network, test-only by
  construction**. So the vault outputs FlowMesh custody depends on cannot appear
  on any real chain today.

**There is NO MPA record type, metadata-cell policy type, or transaction rule
for FlowMesh state**: no "FlowMesh checkpoint/state-root" record, no certified
withdrawal-receipt redemption rule, no vault-release transition. Committing
FlowMesh into B3 means, at minimum:

1. A new frozen MPA type (or cell policy) carrying a FlowMesh state-root /
   certificate commitment, added to the registry with its size/grammar rule
   (`mpa.h:72-106`) and gated like types 4/5 on an activation predicate
   (a new A2/A3-style height — the master handoff's Phase-3 gates — not
   `ModernObjectRulesActive` itself, which is the base F=M switch).
2. Consensus validation of that record: verify the FlowMesh certificate against
   the seat set the chain itself derives — which circles back to the FN seat
   lifecycle (unresolved, §1.6).
3. Withdrawal redemption: `modern::WithdrawalReceipt` exists in the ledger
   (`flowmesh/ledger.h:227`, `modern/vault.h`) but no consensus rule lets a B3
   transaction spend vault custody against a receipt.
4. Deposit recognition (§1.5) plus the DEX_VAULT activation flag becoming a real
   consensus parameter instead of `test_only_asset_policies_active`.

None of this is startable before the owner rules on: FlowMesh finality
(certificate = final? `sync.h:51-57` PROVISIONAL), seat lifecycle, deposit
account-binding, and the activation heights. **Correctly absent, not forgotten.**

### 2.2 Node lifecycle — MISSING (this spike's slice adds a regtest-only version)

There is no FlowMesh member in `node::NodeContext` (`src/node/context.h:57-102`),
no startup call in `src/init.cpp`, no shutdown teardown. The precedent to mirror
is the staking loop: created after chainstate load (`init.cpp:1920-1922`),
stopped in `Interrupt` (`init.cpp:289`), destroyed in `Shutdown`
(`init.cpp:410`), held as `NodeContext::staking` (`context.h:84`).

Needed for any real wiring (and partially delivered by the spike slice, §4):
store path under the datadir, `StartValidator` invocation with a real
`ChainAnchorPolicy`, clean teardown ordering (runtime before `chainman`, since
the anchor policy holds a `ChainstateManager&`).

### 2.3 Transport (microblock networking) — MISSING entirely

The engine is transport-agnostic (`sync.h:49`); every message type is
serializable (`ProposalMsg` `sync.h:116-136`, `AttestationMsg` `:160-169`,
`CertifiedEntry` `:180-197`, `CatchupRequest` `:199-203`) — but:

- No P2P message names in `src/protocol.cpp`/`protocol.h` (grep: zero hits).
- No handling in `src/net_processing.cpp` (zero hits).
- No serialization-window/DoS budget accounting for FlowMesh traffic.
- No peer discovery/addressing for the validator committee.

A real transport must also provide the single-execution-context serialization
MeshNode requires (`sync.h:72-74`) and drive `NoteTimeout` (local liveness
policy, `recovery.h:33`, `sync.h:349-353`). Owner ruling 2026-08-19 requires
Hyperliquid-comparable microblock speed; whether that transport is the B3 P2P
layer at all (vs. a dedicated committee mesh) is an unmade design decision.
Estimated as the second-largest work item after consensus commitment.

### 2.4 RPC surface — MISSING (slice adds one hidden dev RPC)

No `flowmesh` string appears anywhere under `src/rpc/`. An operator surface
would need at least: runtime status (running/halted, sequence, roots, anchors),
action submission (feeding `MeshNode::SubmitAction` → `ActionPool`,
`pool.h:36-58`), market/quorum introspection, and evidence retrieval
(equivocations, certificate conflicts). The Qt shell already anticipates a
backend but renders none (`src/qt/b3assetspage.cpp:340-346`: "no approved
backend"; deposit/withdraw buttons disabled; `b3tradepage` is a shell) — the
API-first ruling (2026-08-23) means these RPCs are the contract the UI waits on.

### 2.5 Activation gating for FlowMesh itself — MISSING by design

`ModernObjectRulesActive` (`era.h:159`) gates modern objects;
`IsActivatedPolicy` (`policy.h:178`) gates policy outputs. FlowMesh has **no
activation predicate at all** because it has no consensus objects to gate (§2.1).
When it gets them, the master handoff's Phase-3 A-heights apply; the governing
rule (CLAUDE.md §2) forbids wiring FlowMesh into consensus before a clean H+1.
The spike slice therefore gates on `ChainType::REGTEST` explicitly and touches
no validation path.

---

## 3. Summary matrix

| Link | Status | Where |
|---|---|---|
| Execution engine (state/clearing/ledger/batch) | EXISTS | `src/flowmesh/{state,clearing,ledger,batch}.h` |
| Microblock identity + re-execution | EXISTS | `src/flowmesh/microblock.h` |
| Certificates, quorum math, recovery locks | EXISTS | `src/flowmesh/{certificate,recovery}.h` |
| Node orchestration (propose/attest/certify/catch-up) | EXISTS | `src/flowmesh/sync.h:236-692` |
| Durable certified log + lock journal + snapshots | EXISTS | `src/node/flowmesh_store.{h,cpp}` |
| Production validator startup (`StartValidator`) | EXISTS, **uncalled** | `src/node/flowmesh_store.cpp:603` |
| Chain-backed anchor policy | EXISTS, uninstantiated | `src/node/flowmesh_anchor.{h,cpp}` |
| Anchor recheck on B3 tip change (validation-signal hook) | MISSING | — |
| Deposit verifier (chain-fact) | PARTIAL: fail-closed stub only | `flowmesh_anchor.h:62-70` |
| DEX_VAULT outputs on a real chain | MISSING (test-only activation flag) | `policy.h:184-186`, `params.h:226` |
| Seat set derivation (FN lifecycle) | MISSING — **owner decision blocks it** | `certificate.h:163-166`, `flowmesh_store.h:31-33` |
| Node lifecycle (init/shutdown/NodeContext) | MISSING → regtest slice in this spike | `init.cpp`, `node/context.h` |
| P2P transport for microblock messages | MISSING | no hits in `protocol.*`, `net_processing.cpp` |
| Timeout/liveness driver | MISSING | `sync.h:349`, `recovery.h:33` |
| RPC surface | MISSING → one hidden dev RPC in this spike | `src/rpc/` |
| FlowMesh state committed into B3 blocks (MPA/cell type) | MISSING | registry `mpa.h:33-41` has no FlowMesh type |
| Withdrawal redemption on B3 | MISSING | receipts exist (`ledger.h:227`) — no consensus rule |
| Activation gating for FlowMesh consensus objects | MISSING by design (Phase-3, post-H+1) | CLAUDE.md rule 2 |
| Qt trade/assets backend | PARTIAL: shell renders "not available" | `qt/b3assetspage.cpp:340-346` |

## 4. The spike slice (regtest-only, node-lifecycle only)

Implemented on this branch, deliberately touching **no** block validation,
mempool policy, or consensus rule:

- `-b3flowmeshdev` (hidden, DEBUG_ONLY): honored only when the chain type is
  REGTEST; any other chain fails init with an explicit error.
- `src/node/flowmesh_dev.{h,cpp}`: `FlowMeshDevRuntime` owning the store
  (`<datadir>/flowmesh-dev/`), a synthetic single-seat market (fixed dev
  constants for vault/base/quote, deterministic dev seat key, threshold from
  `MinCertificateThreshold(1,0)`), a real `ChainAnchorPolicy` at
  `FLOWMESH_ANCHOR_DEPTH`, the fail-closed deposit verifier, and the
  `ValidatorRuntime` from `node::StartValidator`.
- `NodeContext::flowmesh_dev`; created after chainstate load, destroyed in
  `Shutdown` before `chainman`.
- Hidden RPC `getflowmeshinfo`: running/halt state, market ids, quorum,
  sequence, tip hash, state root, current chain anchor.

The slice proves: the production lifecycle starts against a live chainstate,
restores across restarts, and shuts down cleanly. It does NOT propose, attest,
or commit anything (no transport, no peers, no actions).

## 5. Honest effort estimate to production-wire FlowMesh

Blocked-on-owner first: FlowMesh finality model, FN seat lifecycle (+ rotation
in the store), deposit account binding, timeout policy, transport choice,
activation heights, DEX_VAULT real activation. **No engineering below can start
"for real" before those rulings.**

Rough engineering scale once ruled (one experienced dev, review included):

- Consensus commitment (MPA type or cell + validation + withdrawal redemption
  + deposit verifier + vault activation): **6-10 weeks** — the hard, audited part.
- Transport + liveness driver + committee serialization context: **4-8 weeks**
  (more if Hyperliquid-class latency forces a dedicated mesh).
- Seat lifecycle from FN chain state + store rotation support: **3-5 weeks**.
- Node lifecycle hardening + validation-signal anchor hooks: **1-2 weeks**
  (slice exists as the skeleton).
- Operator RPC surface + Qt backend contract: **2-3 weeks**.
- Integration/functional test matrix (multi-node regtest committee): **3-4 weeks**.

Total: **roughly 4-7 developer-months after the owner decisions land**, dominated
by consensus commitment and transport. The engine itself needs essentially no
work — that part is done and tested.

## 6. Spike results

Environment: macOS (Darwin 25.5), `cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo`,
`cmake --build build -j 10`. Branch `experiment/flowmesh-consensus-wiring`
(base `27a6456`).

**Build**: clean before and after the slice (exit 0, no new warnings).

**Unit tests**:

    build/bin/test_bitcoin --run_test=flowmesh_sync_tests,flowmesh_ledger_tests,flowmesh_batch_tests
    Running 40 test cases...  *** No errors detected

**Manual smoke (fresh regtest datadir)**:

1. `b3coind -regtest -b3flowmeshdev -listen=0 -connect=0` — starts;
   `debug.log`: "FlowMesh dev validator started (REGTEST spike):
   store=<datadir>/regtest/flowmesh-dev sequence=0".
2. `b3coin-cli -regtest getflowmeshinfo` — `running: true`, `halt: "none"`,
   `threshold: 1`, `sequence: 0`, all-zero `last_hash`, non-trivial
   `state_root` (the canonical empty market state), derived
   `execution_config_id`, `current_anchor: {height: -1, hash: 0x0}` (chain
   shorter than `FLOWMESH_ANCHOR_DEPTH`).
3. After `generatetoaddress 40 ...`: `current_anchor` reported
   `{height: 10, hash: <block 10>}` — tip 40 minus the ratified depth 30;
   the real `ChainAnchorPolicy` tracks the live chainstate.
4. `b3coin-cli stop` — clean shutdown ("Shutdown done"), runtime destroyed
   before chainman.
5. Restart with the same datadir — `StartValidator` reopened the existing
   store (marker/domain/quorum revalidated, empty log replayed):
   `running: true`, `halt: "none"`, `sequence: 0`.
6. Negative gate: `b3coind -testnet -b3flowmeshdev -networkactive=0
   -listen=0 -connect=0` refused init:
   "Error: -b3flowmeshdev is a regtest-only development option and must
   never run on this chain." (checked in `AppInitParameterInteraction`,
   before any subsystem starts).

What the slice deliberately does NOT do: no proposing/attesting/committing
(no transport drives the MeshNode), no deposits (fail-closed verifier), no
block validation, mempool, or consensus involvement of any kind.

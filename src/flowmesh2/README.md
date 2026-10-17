# Clean-core FlowMesh foundations — C1

New C++ implementation beside V1, not a wrapper around the old runtime.
This first milestone is **infrastructure only**, disabled by default and not
linked into the node, wallet or Qt. It does not execute trades.

- `BoundedMailbox`: opaque FIFO, count and accounted-payload limits, rejection
  preserves input, close refuses new work while retaining queued originals.
- `ReadSlot`: deeply owned one-head opaque snapshots and move-only bounded
  reader leases; readers do not acquire publisher, chain or market locks.
- Actual Bitcoin `Mutex`/`LOCK` and compiler-enforced lock annotations.

Neither component authenticates an action, certifies a result, grants spending
authority or acknowledges disk durability. Read publication is memory visibility
only. Accounted payload length is not a total-heap/DoS bound; caller allocations,
capacity, concurrency and CPU budgets remain integration requirements.

See the [replacement roadmap](../../doc/design/b3-flowmesh-clean-core-20261016.md),
[versioned test profile](TEST_PROFILE.json) and [C1 results](C1_RESULTS.md).

## Reproduce

Use a clean committed checkout, not the unrelated dirty native spot prototype.
Qualified source: `1f05fc748c1c74ad7afff4e63ab21dc81bfb8c88`.
No node, wallet, network, keys or existing datadir are used.

```sh
cmake -S . -B build-flowmesh2-c1 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_FLOWMESH2_FOUNDATION_TESTS=ON \
  -DBUILD_TESTS=ON -DBUILD_GUI=OFF -DBUILD_DAEMON=OFF -DBUILD_CLI=OFF \
  -DBUILD_TX=OFF -DBUILD_UTIL=OFF -DBUILD_WALLET_TOOL=OFF \
  -DENABLE_WALLET=OFF -DENABLE_IPC=OFF -DWITH_CCACHE=OFF
cmake --build build-flowmesh2-c1 --target test_flowmesh2_foundations --parallel 4
ctest --test-dir build-flowmesh2-c1 -R '^flowmesh2_foundations$' --no-tests=error --verbose
```

Expected: one CTest executable, **19 internal cases** (12 mailbox + 7 read-slot),
no skips. The executable rejects a changed case count and runs checks even under
`NDEBUG`. Each CTest execution has a 60-second deadline. Compilation uses
`-Wthread-safety -Werror=thread-safety` on supported Clang compilers.

This run qualifies neither full V1 nor full V2. The 200 ms target must later
include original preflight, actual crypto/agreement, durable execution and
durable client verification on isolated regtest. Test runtime is not trading
latency.

## Rebirth outline (branch `rebirth`)

The C1 text above is unchanged and C1 stays frozen. On branch `rebirth` the
whole new core is being laid out as a walking skeleton (slices R1 to R14) that
Codex then completes:

- [ARCHITECTURE.md](ARCHITECTURE.md): module layout, data flow, lock and
  determinism rules, durability barriers, authority versioning, what is real
  versus stubbed, how to build and test, and the walking-skeleton acceptance.
- [TODO.md](TODO.md): the task ledger. Every `// TODO(rebirth:<ID>)` tag in the
  source names a row there, with its status, acceptance test, evidence path and
  the provisional decisions D1 to D9.

Landed so far: **R1**, the `BUILD_FLOWMESH2` gate (default OFF; it requires
`BUILD_TESTS` and `BUILD_FLOWMESH2_FOUNDATION_TESTS`), the `[[nodiscard]]`
`Result`/`Error` model and code registries in `core/`, the Boost.Test binary
`test_flowmesh2` with its case inventory `test/INVENTORY.txt`, and the lint
scripts `contrib/flowmesh2/check_inventory.py` and `check_todo.py`; and
**R2**, the seven guards `contrib/flowmesh2/check_guards.py` with their
fixture self-test, the compile-fail controls in `test/compile_fail/`, the
evidence bundle tool `contrib/flowmesh2/evidence.py`, the sanitizer canaries
(`-DFLOWMESH2_SANITIZER_CANARIES=ON`, default OFF) and the owner-protected CI
workflow `.github/workflows/flowmesh2.yml`, which the outline never runs;
and **R3**, the deterministic core primitives in `core/` (checked amounts, id
types, the `B3/FM2/` tag registry, the exact bounded codec, the provisional
regtest profile with `PROFILE_DEX1_REGTEST.json`, authority types, size-bound
tagged Merkle trees, market descriptors and the slot header); and **R4**, the
signed `ActionV2` with BIP340 verification and `VerifiedAction`, the slot
body, certified outcomes, chain facts (deposits real, five later kinds
stubbed), slot certificates, PoP-checked seat sets, the commitment v0
account leaf and the shared nonce-window and fee spec functions (test keys
in `test/util/keys.h`; the test binary links `bitcoin_common` for `CKey`
only); and **R5**, the first application library `fm2_core` with the shared
spot ledger in `ledger/` (one balance per account and asset across all
markets, nonce windows, fee withholding, atomic operations and a
conservation check after every operation) and state commitment v0 with
account proofs, plus the commitment value types and protocol accounts in
`core/state_root`; and **R6**, the matching engine's first half in
`engine/`: the provisional order book (price-time FIFO, exact
reservations, never crossed), the A2 sweep of same-slot takers (pro-rata
per level by largest remainder, pairing by canonical rank against FIFO
makers, execution at the maker price, the seller fee on each order's
cumulative quote, per-order and per-slot fill caps) that settles through
the ledger, residual-cross detection, and a naive reference matcher in
`test/` that 1,000 seeded batches are compared against; and **R7**, the
slot engine in `engine/engine`: the canonical order of a slot's originals
(seeded by the parent header, each account's actions in nonce order), the
slot validity predicate whose only product is a `ValidatedSlot`, the
engine state with its commitment (now including every resting order's
leaf) and slot-boundary invariants, and `Engine::ApplySlot`, which runs
deposits, transfers, cancels, post-only orders, one joint A2 sweep per
market, resting, fees and a conservation check, and certifies one outcome
per fact and action; and **R8**, durable storage in `storage/`: framed,
CRC-checked and hash-chained journal records, a memory journal with a
fault plan, a file journal on a pre-zeroed segment with strict commits
(`fdatasync`, or `F_FULLFSYNC` on macOS, every error sticky and fatal),
durability tokens that only `Journal::Confirm` mints after a barrier, the
signer log (accepted proposals, intents and signed votes with the
equivocation rule), the seat signer whose `Sign` takes only a
`DurableIntent`, the commit log (decisions with their bodies, and
applications) and `agreement/outbound.h`; reopening a file journal,
rollover, group commit, snapshots, folded signing and the PBFT view-state
records are typed stubs; and **R9**, the state owner in `state/`: the core
state and its genesis, owned committed heads, slot candidates that only
the engine can produce (with the originals in canonical order, so a
permuted proposal gives the same header), the single-writer `StateOwner`
whose `Preview` and `Reexecute` run without its lock and whose `Apply`
installs a candidate only under a `DurableDecision` of its commit journal
and the current authority, and `ReplayDecisions`, which rebuilds every
header and the final state from the commit journal; restoring a head,
pipelined previews and the snapshot and view-state records are typed
stubs; and **R10**, the chain adapter seam in `chain/` (`Poll` answers
facts, BUSY, NEED_DATA or AUTHORITY_CHANGED with a rewind cursor, and every
batch must pass `CheckBatch`), the capped agreement messages and the
`Agreement` and `SafetyRules` interfaces in `agreement/`, and the new
REGTEST-only `fm2_dev` library with a scripted fake chain and the k = 1
dev certifier, which writes `ACCEPTED_PROPOSAL`, `INTENT`, `SIGNED_VOTE`
and `DECISION` in barrier order with both authority checks; PBFT, the
leader schedule, inclusion lists, the restart fence, the evidence store
and the b3d adapters are typed stubs; and **R11**, certified reads in
`read/`: an immutable `SlotView` per committed slot (header, certificate,
outcomes, v0 account leaves with their tree levels, action and order-fill
indexes), and the `ReadService`, which publishes only after B-APPLIED
(`Publish` takes the slot's `DurableApplied` token) a header-and-certificate
envelope into the C1 `ReadSlot` and answers `Account`, `Action` (INCLUDED,
EXPIRED only with a proof, or UNKNOWN) and `OrderFills` from one lease per
reply with the label `CERTIFIED`; and b3verify in the new library
`fm2_verify`, which links no application code and checks certificates,
outcomes, leaves, fills, balance transitions and EXPIRED proofs with the
engine's own spec functions; liability paths, checkpoints and anchor proofs
are typed stubs; and **R12**, admission in `admission/`: A0..A10 against
one head snapshot (the account, nonce window, expiry and budget are
checked before the BIP340 signature, and an exact resend of a pending or
retained included action is `DUPLICATE` with no signature check), with
per-code counters and a sticky fence, and the bounded `ActionPool`
(`IN_PROPOSAL` without removal, terminal `NONCE_USED`, `NONCE_TOO_LOW` and
`EXPIRED`, a taken slot that always passes the validity predicate); credit
budgets and batch verification are typed stubs, and `fm2_dev` adds the
REGTEST unlimited dev budget; and **R13**, the service lanes in `net/`
(four C1 mailboxes in strict priority CRITICAL > PROPOSAL > ACTION > BULK,
with a reserved tenth of every lane for cancel, status and health, and
BUSY that always means "not queued"), the client API contract and the
in-process API (exactly one reply per request, admission through the
lane, and `GetAction` from one read lease that answers EXPIRED only with
a proof), and the seat mesh interface whose only vote path takes a
durable vote; HTTPS + SSE, streams, the real meshes, short IDs, sentry
fan-out and the `bridge/`, `futures/` and `governance/` boundaries are
typed stubs. Nothing is linked into the node, wallet or Qt.

```sh
git -C /Users/josh/development/ON/B3-FlowMesh-hl branch --show-current   # must print: rebirth
cmake -S . -B build-rebirth -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTS=ON -DBUILD_FLOWMESH2_FOUNDATION_TESTS=ON -DBUILD_FLOWMESH2=ON \
  -DBUILD_GUI=OFF -DBUILD_DAEMON=OFF -DBUILD_CLI=OFF -DBUILD_TX=OFF -DBUILD_UTIL=OFF \
  -DBUILD_WALLET_TOOL=OFF -DENABLE_WALLET=OFF -DENABLE_IPC=OFF -DWITH_CCACHE=OFF
cmake --build build-rebirth --target fm2_all --parallel 4
ctest --test-dir build-rebirth -L flowmesh2 --no-tests=error --output-on-failure
```

Expected after R13: 8 CTests pass (`flowmesh2_foundations` with its 19 C1
cases, `flowmesh2_unit` with 476 cases, `flowmesh2_inventory`,
`flowmesh2_todo_lint`, `flowmesh2_guards`, `flowmesh2_guards_selftest`,
`flowmesh2_evidence_selftest` and, with Clang, `flowmesh2_negative_compile`
with four controls). The file journal tests write only under
`build-rebirth/fm2-test-tmp/`.
Sanitizer builds and their canaries are described in ARCHITECTURE.md §13.2. Open gates (workspace lock reconciliation, decision
D3) are listed in ARCHITECTURE.md §0. All outline values are provisional and
REGTEST-only.
